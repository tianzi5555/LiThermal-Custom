// LiOS「BW21 摄像头」应用（插件）
//
// 作用：通过 USB-C 双头线连着的 BW21（RTL8735B，带 1080p30 摄像头）取画面显示。
//
// 链路（全部用户态，不改内核、不改系统任何文件）：
//   [相机] USB gadget = CDC-ECM（configfs 里给已有的 g1 追加 ecm.usb0）→ 网卡 usbnet_ecm
//          IP 192.168.2.231/24
//   [BW21] USB Host = usbh_cdc_ecm → ECM 网口 IP 192.168.2.250/24
//          摄像头 → ISP → JPEG 编码 → MMF2 → TCP 服务端 :5555
//   协议：每帧 = [4 字节大端长度][JPEG 数据]
//
// 纪律：所有 socket / 文件 / system() 都在 worker 线程；LVGL 回调与 timer 里只换缓冲、只改标签。
// 注意：启用 ECM 会占用 USB 口 → adb 会断（这是硬件只有一个 OTG 口的必然取舍），
//       所以第一次启用前弹确认框；长按确认键可恢复 adb 模式。

#include "lios_app.h"
#include "my_main.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

extern "C" void lios_log(const char *fmt, ...);

// ---------------------------------------------------------------- 配置
// 链路地址：必须和 BW21 固件 li_video_stream.c 里的 VS_IP_HOST 一致。
// 用独立的 192.168.77.x —— BW21 开机还会连家里 WiFi，若 ECM 口和 WiFi 同网段，
// lwIP 会按 netif_list 顺序把发给相机的包从 WiFi 口送出去（本方案不打 lwIP 补丁）。
#define BW21_IP "192.168.77.1"
#define BW21_PORT 5555
#define CAM_IP "192.168.77.2"
#define CAM_MASK "255.255.255.0"
#define VW 320 // 显示缓冲宽
#define VH 240 // 显示缓冲高
#define JPEG_MAX (600 * 1024)

// ---------------------------------------------------------------- 共享状态
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static volatile bool s_run = false;
static volatile bool s_started = false;
static volatile bool s_frozen = false;
static volatile bool s_req_reconnect = false;
static volatile bool s_req_enable_usb = false;
static volatile bool s_req_restore_adb = false;

static char s_status[112] = "未连接";
static char s_detail[112] = "";
static int s_fps = 0;
static int s_w = 0, s_h = 0;

// 解码后的 BGRA 双缓冲（worker 写后台，UI 换到前台）
static uint8_t *s_buf[2] = {NULL, NULL};
static int s_front = 0;      // 当前 canvas 用的
static volatile int s_ready = -1; // worker 填好的缓冲下标（-1 = 无）

// ---------------------------------------------------------------- LVGL
static lv_obj_t *g_root = NULL;
static lv_obj_t *g_canvas = NULL;
static lv_obj_t *g_status = NULL;
static lv_obj_t *g_detail = NULL;
static lv_obj_t *g_hint = NULL;
static lv_obj_t *g_dialog = NULL;
static lv_obj_t *g_dlg_btn[2] = {NULL, NULL};
static int g_dlg_sel = 0;
static int g_dlg_action = 0; // 1=启用USB网卡 2=恢复adb
static lv_timer_t *g_timer = NULL;

static int g_left = 0, g_right = 0, g_enter = 0, g_rec = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static int g_enter_long = 0; // 长按确认的待处理次数（不能是 bool：要 ++/--）

// ---------------------------------------------------------------- 小工具（worker 线程用）
static int run_sh(const char *cmd)
{
    int r = system(cmd);
    lios_log("bw21cam: system(%s) -> %d", cmd, r);
    return r;
}

// 找 ECM gadget 网卡名：没有 device 符号链接的那个（真 USB 网卡如 usb0 有 device）
static int find_ecm_ifname(char *out, size_t n)
{
    DIR *d = opendir("/sys/class/net");
    if (d == NULL)
        return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        if (strcmp(e->d_name, "lo") == 0)
            continue;
        char p[160];
        snprintf(p, sizeof(p), "/sys/class/net/%s/device", e->d_name);
        if (access(p, F_OK) == 0)
            continue; // 有 device = 真 USB 设备（热像模组的 usb0），跳过
        snprintf(out, n, "%s", e->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

// 读一个小文件（worker 线程用）
static void read_small(const char *path, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
    size_t l = strlen(out);
    while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' '))
        out[--l] = 0;
}

static void link_diag(void); // 前向声明：enable_ecm_gadget 里要用，定义在后面

// 启用 ECM gadget —— 完全照搬在这台相机上验证过的 usbnet_boot.sh 顺序：
//   等 gadget 就绪 → 解绑 → sleep 1 → 摘 MTP + ADB（T113 的 UDC 端点很少，
//   ECM 要独占；MTP 占 3 个端点、adb 占 2 个，不摘会导致主机 SET_INTERFACE 失败，
//   BW21 那边表现为 "ECM alt setting err"，网卡永远起不来）
//   → 建 function → 链接进 c.1 → 绑定 UDC → sleep 1 → 验证绑定成功
// 每步结果写 /tmp/bw21_ecm.stat 并显示在屏幕上（ECM 模式下没有 adb，只能靠屏幕看）。
static void enable_ecm_gadget(void)
{
    pthread_mutex_lock(&s_mtx);
    snprintf(s_status, sizeof(s_status), "正在配置 USB 网卡…");
    snprintf(s_detail, sizeof(s_detail), "");
    pthread_mutex_unlock(&s_mtx);

    run_sh("S=/tmp/bw21_ecm.stat; G=/sys/kernel/config/usb_gadget/g1; "
           "echo start > $S; "
           "i=0; udc=''; "
           "while [ $i -lt 20 ]; do "
           "  if [ -e \"$G/configs/c.1/ffs.adb\" ]; then udc=$(cat \"$G/UDC\" 2>/dev/null); fi; "
           "  if [ -n \"$udc\" ]; then break; fi; "
           "  sleep 1; i=$((i+1)); "
           "done; "
           "if [ -z \"$udc\" ]; then echo 'fail:gadget未就绪(UDC为空)' > $S; exit 1; fi; "
           "echo \"udc=$udc\" > $S; "
           "echo '' > \"$G/UDC\" 2>/dev/null; "
           "sleep 1; "
           "if [ -e \"$G/configs/c.1/ffs.mtp\" ]; then rm -f \"$G/configs/c.1/ffs.mtp\"; fi; "
           "if [ -e \"$G/configs/c.1/ffs.adb\" ]; then rm -f \"$G/configs/c.1/ffs.adb\"; fi; "
           "if [ ! -d \"$G/functions/ecm.usb0\" ]; then mkdir \"$G/functions/ecm.usb0\"; fi; "
           "if [ ! -d \"$G/functions/ecm.usb0\" ]; then echo 'fail:建ecm功能失败' > $S; exit 1; fi; "
           "echo 02:00:00:00:ec:01 > \"$G/functions/ecm.usb0/dev_addr\" 2>/dev/null; "
           "echo 02:00:00:00:ec:02 > \"$G/functions/ecm.usb0/host_addr\" 2>/dev/null; "
           "echo usbnet_ecm > \"$G/functions/ecm.usb0/ifname\" 2>/dev/null; "
           "if [ ! -e \"$G/configs/c.1/ecm.usb0\" ]; then "
           "  ln -s \"$G/functions/ecm.usb0\" \"$G/configs/c.1/ecm.usb0\" 2>/dev/null; "
           "fi; "
           "if [ ! -e \"$G/configs/c.1/ecm.usb0\" ]; then echo 'fail:链接进c.1失败(端点不足?)' > $S; exit 1; fi; "
           "echo \"$udc\" > \"$G/UDC\" 2>/dev/null; "
           "sleep 1; "
           "if [ -z \"$(cat \"$G/UDC\" 2>/dev/null)\" ]; then echo 'fail:绑定UDC失败' > $S; exit 1; fi; "
           "echo ok > $S");

    char st[80];
    read_small("/tmp/bw21_ecm.stat", st, sizeof(st));
    lios_log("bw21cam: gadget 配置结果 = %s", st);

    // 等网卡出现并配 IP（最多 25 秒）。
    // 识别方式：优先读 gadget 里我们写进去的 ifname；读不到就排除法扫描
    // （排除 lo / usb0=热像模组 / wlan0）—— 之前用「有没有 device 符号链接」判断
    // gadget 网卡不成立，会一直认不出来。
    // 结果写 /tmp/bw21_ifname，网卡清单写 /tmp/bw21_iflist（屏幕上要显示，ECM 模式下没有 adb）。
    run_sh("G=/sys/kernel/config/usb_gadget/g1; "
           "ls /sys/class/net 2>/dev/null | tr '\\n' ',' > /tmp/bw21_iflist; "
           "n=$(cat \"$G/functions/ecm.usb0/ifname\" 2>/dev/null); "
           "if [ -n \"$n\" ] && [ -e \"/sys/class/net/$n\" ]; then :; else "
           "  n=''; "
           "  for i in $(ls /sys/class/net 2>/dev/null); do "
           "    case \"$i\" in lo|usb0|wlan0|sit0|tunl0) continue ;; esac; "
           "    n=\"$i\"; break; "
           "  done; "
           "fi; "
           "if [ -n \"$n\" ]; then "
           "  ifconfig \"$n\" " CAM_IP " netmask " CAM_MASK " up 2>/dev/null; "
           "  echo \"$n\" > /tmp/bw21_ifname; "
           "else "
           "  echo '' > /tmp/bw21_ifname; "
           "fi");

    char ifn[32] = "";
    char iflist[96] = "";
    for (int i = 0; i < 25; i++)
    {
        read_small("/tmp/bw21_ifname", ifn, sizeof(ifn));
        if (ifn[0] != 0)
            break;
        // 还没出现：重新扫一遍（顺便刷新清单）
        run_sh("G=/sys/kernel/config/usb_gadget/g1; "
               "ls /sys/class/net 2>/dev/null | tr '\\n' ',' > /tmp/bw21_iflist; "
               "n=''; "
               "for i in $(ls /sys/class/net 2>/dev/null); do "
               "  case \"$i\" in lo|usb0|wlan0|sit0|tunl0) continue ;; esac; "
               "  n=\"$i\"; break; "
               "done; "
               "if [ -n \"$n\" ]; then "
               "  ifconfig \"$n\" " CAM_IP " netmask " CAM_MASK " up 2>/dev/null; "
               "  echo \"$n\" > /tmp/bw21_ifname; "
               "else echo '' > /tmp/bw21_ifname; fi");
        read_small("/tmp/bw21_ifname", ifn, sizeof(ifn));
        if (ifn[0] != 0)
            break;
        read_small("/tmp/bw21_iflist", iflist, sizeof(iflist));
        pthread_mutex_lock(&s_mtx);
        snprintf(s_status, sizeof(s_status), "等网卡出现… %d/25", i + 1);
        snprintf(s_detail, sizeof(s_detail), "现有网卡:%s", iflist[0] ? iflist : "?");
        pthread_mutex_unlock(&s_mtx);
        sleep(1);
    }
    read_small("/tmp/bw21_iflist", iflist, sizeof(iflist));

    if (ifn[0] == 0)
    {
        pthread_mutex_lock(&s_mtx);
        snprintf(s_status, sizeof(s_status), "USB 网卡未启用");
        if (strncmp(st, "ok", 2) != 0 && st[0] != 0)
            snprintf(s_detail, sizeof(s_detail), "%s", st);
        else
            snprintf(s_detail, sizeof(s_detail), "无新网卡 现有:%s", iflist[0] ? iflist : "?");
        pthread_mutex_unlock(&s_mtx);
        return;
    }

    // 配好立刻做一次链路诊断（ifconfig 真实状态 + ping BW21），
    // 不等 connect —— 否则用户只能看到"连接中"，不知道链路到底通不通。
    link_diag();
    char diag[96];
    read_small("/tmp/bw21_diag.txt", diag, sizeof(diag));

    pthread_mutex_lock(&s_mtx);
    snprintf(s_status, sizeof(s_status), "网卡 %s 就绪，连接中…", ifn);
    snprintf(s_detail, sizeof(s_detail), "%s", diag[0] ? diag : (ifn));
    pthread_mutex_unlock(&s_mtx);
    lios_log("bw21cam: ECM 网卡就绪 %s = " CAM_IP " 诊断: %s", ifn, diag);
}

// 恢复 adb 模式（摘掉 ecm，挂回 ffs.adb + ffs.mtp；configfs 是内存态，重启也能复原）
static void restore_adb(void)
{
    pthread_mutex_lock(&s_mtx);
    snprintf(s_status, sizeof(s_status), "正在恢复 adb…");
    pthread_mutex_unlock(&s_mtx);
    run_sh("S=/tmp/bw21_ecm.stat; G=/sys/kernel/config/usb_gadget/g1; "
           "udc=$(cat \"$G/UDC\" 2>/dev/null); "
           "if [ -z \"$udc\" ]; then udc=$(ls /sys/class/udc 2>/dev/null | head -1); fi; "
           "echo '' > \"$G/UDC\" 2>/dev/null; "
           "sleep 1; "
           "rm -f \"$G/configs/c.1/ecm.usb0\" 2>/dev/null; "
           "if [ -d \"$G/functions/ffs.adb\" ] && [ ! -e \"$G/configs/c.1/ffs.adb\" ]; then "
           "  ln -s \"$G/functions/ffs.adb\" \"$G/configs/c.1/ffs.adb\" 2>/dev/null; "
           "fi; "
           "if [ -d \"$G/functions/ffs.mtp\" ] && [ ! -e \"$G/configs/c.1/ffs.mtp\" ]; then "
           "  ln -s \"$G/functions/ffs.mtp\" \"$G/configs/c.1/ffs.mtp\" 2>/dev/null; "
           "fi; "
           "echo \"$udc\" > \"$G/UDC\" 2>/dev/null; "
           "sleep 1; "
           "if [ -n \"$(cat \"$G/UDC\" 2>/dev/null)\" ]; then echo restored > $S; else echo 'fail:恢复绑定失败' > $S; fi");
    char st[64];
    read_small("/tmp/bw21_ecm.stat", st, sizeof(st));
    pthread_mutex_lock(&s_mtx);
    snprintf(s_status, sizeof(s_status), "已尝试恢复 adb");
    snprintf(s_detail, sizeof(s_detail), "%s",
             strncmp(st, "restored", 8) == 0 ? "电脑看不到设备就重启相机" : st);
    pthread_mutex_unlock(&s_mtx);
}

static bool have_ecm_iface(void)
{
    char tmp[32];
    return find_ecm_ifname(tmp, sizeof(tmp)) == 0;
}

// 链路诊断：把「网卡真实状态 + 能不能 ping 通 BW21」写成一行，显示到屏幕上。
// ECM 模式下 adb 已经断了，只能靠屏幕看，所以这一行必须够用。
static void link_diag(void)
{
    run_sh("n=$(cat /tmp/bw21_ifname 2>/dev/null); "
           "if [ -z \"$n\" ]; then echo '无网卡' > /tmp/bw21_diag.txt; exit 0; fi; "
           "o=$(ifconfig \"$n\" 2>&1); "
           "ip=$(echo \"$o\" | grep -o 'inet addr:[0-9.]*' | head -1 | cut -d: -f2); "
           "case \"$o\" in *UP*) u=UP ;; *) u=DOWN ;; esac; "
           "case \"$o\" in *RUNNING*) r=RUN ;; *) r=NOCARRIER ;; esac; "
           "p=$(ping -c 2 -W 1 " BW21_IP " 2>&1 | grep -o '[0-9]*% packet loss' | head -1); "
           "echo \"$n $u/$r ${ip:-无IP} ping:${p:-失败}\" > /tmp/bw21_diag.txt");
}

// ---------------------------------------------------------------- 收流 + 解码

// 带超时的 connect：SO_RCVTIMEO 管不到 connect()，对端不应答 ARP 时
// 阻塞式 connect 会按内核默认 SYN 重试卡住约 2 分钟 —— 所以用非阻塞 + select。
static bool connect_with_timeout(int fd, const struct sockaddr *sa, socklen_t len, int timeout_ms)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        fl = 0;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int r = connect(fd, sa, len);
    if (r == 0)
    {
        fcntl(fd, F_SETFL, fl);
        return true;
    }
    if (errno != EINPROGRESS)
    {
        fcntl(fd, F_SETFL, fl);
        return false;
    }

    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    FD_ZERO(&efds);
    FD_SET(fd, &efds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int sel = select(fd + 1, NULL, &wfds, &efds, &tv);
    bool ok = false;
    if (sel > 0 && FD_ISSET(fd, &wfds) && !FD_ISSET(fd, &efds))
    {
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        ok = (err == 0);
    }
    fcntl(fd, F_SETFL, fl); // 恢复阻塞模式，后面 recv 要用 SO_RCVTIMEO
    return ok;
}

static bool read_all(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0)
        {
            if (r < 0 && errno == EINTR)
                continue;
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

static void stream_loop(void)
{
    AVCodecContext *dec = NULL;
    AVFrame *frm = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    int sws_w = 0, sws_h = 0, sws_fmt = -1;
    uint8_t *jpeg = (uint8_t *)malloc(JPEG_MAX);
    if (jpeg == NULL)
        return;

    int fail_cnt = 0;
    while (s_run)
    {
        if (s_req_enable_usb)
        {
            s_req_enable_usb = false;
            enable_ecm_gadget();
        }
        if (s_req_restore_adb)
        {
            s_req_restore_adb = false;
            restore_adb();
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            usleep(500 * 1000);
            continue;
        }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(BW21_PORT);
        inet_pton(AF_INET, BW21_IP, &sa.sin_addr);

        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        pthread_mutex_lock(&s_mtx);
        snprintf(s_status, sizeof(s_status), "连接 BW21 %s:%d …", BW21_IP, BW21_PORT);
        pthread_mutex_unlock(&s_mtx);

        if (!connect_with_timeout(fd, (struct sockaddr *)&sa, sizeof(sa), 4000))
        {
            close(fd);
            fail_cnt++;
            link_diag(); // 把网卡真实状态 + ping 结果写到 /tmp/bw21_diag.txt
            char diag[96];
            read_small("/tmp/bw21_diag.txt", diag, sizeof(diag));
            pthread_mutex_lock(&s_mtx);
            if (!have_ecm_iface())
            {
                snprintf(s_status, sizeof(s_status), "USB 网卡未启用");
                snprintf(s_detail, sizeof(s_detail), "按确认键启用（会断开 adb）");
            }
            else
            {
                snprintf(s_status, sizeof(s_status), "连不上 BW21 (%d)", fail_cnt);
                snprintf(s_detail, sizeof(s_detail), "%s", diag[0] ? diag : "诊断无输出");
            }
            pthread_mutex_unlock(&s_mtx);
            usleep(2000 * 1000);
            continue;
        }
        fail_cnt = 0;

        pthread_mutex_lock(&s_mtx);
        snprintf(s_status, sizeof(s_status), "已连接，等待画面…");
        snprintf(s_detail, sizeof(s_detail), "");
        pthread_mutex_unlock(&s_mtx);
        lios_log("bw21cam: 已连接 BW21");

        if (dec == NULL)
        {
            const AVCodec *cd = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            if (cd != NULL)
            {
                dec = avcodec_alloc_context3(cd);
                if (dec != NULL && avcodec_open2(dec, cd, NULL) < 0)
                {
                    avcodec_free_context(&dec);
                    dec = NULL;
                }
            }
            if (dec == NULL)
            {
                pthread_mutex_lock(&s_mtx);
                snprintf(s_status, sizeof(s_status), "解码器初始化失败");
                pthread_mutex_unlock(&s_mtx);
                close(fd);
                break;
            }
            frm = av_frame_alloc();
            pkt = av_packet_alloc();
        }

        // 帧循环
        long frames = 0;
        uint32_t fps_t0 = (uint32_t)time(NULL);
        while (s_run)
        {
            if (s_req_reconnect)
            {
                s_req_reconnect = false;
                break;
            }
            uint8_t hdr[4];
            if (!read_all(fd, hdr, 4))
                break;
            uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                           ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
            if (len < 64 || len > JPEG_MAX)
            {
                lios_log("bw21cam: 帧长异常 %u，重连", (unsigned)len);
                break;
            }
            if (!read_all(fd, jpeg, len))
                break;

            frames++;
            uint32_t nows = (uint32_t)time(NULL);
            if (nows != fps_t0)
            {
                s_fps = (int)frames;
                frames = 0;
                fps_t0 = nows;
                pthread_mutex_lock(&s_mtx);
                snprintf(s_status, sizeof(s_status), "已连接 %dx%d", s_w, s_h);
                pthread_mutex_unlock(&s_mtx);
            }

            if (s_frozen)
                continue; // 冻结：仍读掉数据保持同步，只是不解码显示

            pkt->data = jpeg;
            pkt->size = (int)len;
            if (avcodec_send_packet(dec, pkt) < 0)
                continue;
            if (avcodec_receive_frame(dec, frm) != 0)
                continue;

            int w = frm->width, h = frm->height;
            if (w <= 0 || h <= 0)
                continue;

            // 等比缩放到 VW x VH 内
            int dw = VW, dh = VH;
            if (w * VH > h * VW)
            {
                dw = VW;
                dh = VW * h / w;
            }
            else
            {
                dh = VH;
                dw = VH * w / h;
            }
            if (dw <= 0)
                dw = 2;
            if (dh <= 0)
                dh = 2;

            if (sws == NULL || sws_w != w || sws_h != h || sws_fmt != frm->format)
            {
                if (sws != NULL)
                    sws_freeContext(sws);
                sws = sws_getContext(w, h, (enum AVPixelFormat)frm->format, dw, dh,
                                     AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
                sws_w = w;
                sws_h = h;
                sws_fmt = frm->format;
                if (sws == NULL)
                    continue;
            }

            int back = s_front ^ 1;
            if (s_buf[back] == NULL)
                continue;
            uint8_t *dst[4] = {s_buf[back], NULL, NULL, NULL};
            int dstst[4] = {dw * 4, 0, 0, 0};

            // 不足整屏的部分留黑（只在尺寸变化时清一次）
            static int last_dw = 0, last_dh = 0;
            if (dw != last_dw || dh != last_dh)
            {
                memset(s_buf[0], 0, (size_t)VW * VH * 4);
                memset(s_buf[1], 0, (size_t)VW * VH * 4);
                last_dw = dw;
                last_dh = dh;
            }
            // 垂直居中
            uint8_t *d = s_buf[back] + (size_t)((VH - dh) / 2) * VW * 4 + (size_t)((VW - dw) / 2) * 4;
            dst[0] = d;

            sws_scale(sws, frm->data, frm->linesize, 0, h, dst, dstst);

            s_w = w;
            s_h = h;
            s_ready = back;
        }
        close(fd);
        pthread_mutex_lock(&s_mtx);
        snprintf(s_status, sizeof(s_status), "连接断开");
        pthread_mutex_unlock(&s_mtx);
        usleep(500 * 1000);
    }

    if (sws != NULL)
        sws_freeContext(sws);
    if (frm != NULL)
        av_frame_free(&frm);
    if (pkt != NULL)
        av_packet_free(&pkt);
    if (dec != NULL)
        avcodec_free_context(&dec);
    free(jpeg);
}

static void *worker(void *)
{
    stream_loop();
    s_started = false;
    return NULL;
}

// ---------------------------------------------------------------- UI
static void open_dialog(int action)
{
    if (g_dialog != NULL && lv_obj_is_valid(g_dialog))
        return;
    g_dlg_action = action;
    g_dlg_sel = 0;
    g_dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_dialog, 290, 96);
    lv_obj_align(g_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(g_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(g_dialog, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_dialog, lv_color_hex(0x0B1116), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_dialog, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_dialog, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(g_dialog, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(g_dialog);

    lv_obj_t *t = lv_label_create(g_dialog);
    lv_obj_set_style_text_font(t, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(t, action == 1 ? "启用 USB 网卡？（adb 会断开）" : "恢复 adb 模式？");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

    static const char *labs[2] = {"确认", "取消"};
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *b = lv_btn_create(g_dialog);
        lv_obj_set_size(b, 100, 30);
        lv_obj_set_pos(b, 26 + i * 118, 52);
        lv_obj_set_style_radius(b, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(l, labs[i]);
        lv_obj_center(l);
        g_dlg_btn[i] = b;
    }
    lv_obj_set_style_bg_color(g_dlg_btn[0], lv_color_hex(0x2A4A68), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void close_dialog(void)
{
    if (g_dialog != NULL && lv_obj_is_valid(g_dialog))
        lv_obj_del(g_dialog);
    g_dialog = NULL;
    g_dlg_btn[0] = g_dlg_btn[1] = NULL;
}

static void dlg_move(int dir)
{
    if (g_dialog == NULL)
        return;
    lv_obj_set_style_bg_color(g_dlg_btn[g_dlg_sel], lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
    g_dlg_sel = (g_dlg_sel + dir + 2) % 2;
    lv_obj_set_style_bg_color(g_dlg_btn[g_dlg_sel], lv_color_hex(0x2A4A68), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void dlg_ok(void)
{
    int act = g_dlg_action;
    bool yes = (g_dlg_sel == 0);
    close_dialog();
    if (!yes)
        return;
    if (act == 1)
        s_req_enable_usb = true;
    else if (act == 2)
        s_req_restore_adb = true;
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    // 换缓冲（worker 已填好）
    int r = s_ready;
    if (r >= 0 && g_canvas != NULL && lv_obj_is_valid(g_canvas) && s_buf[r] != NULL)
    {
        s_ready = -1;
        s_front = r;
        lv_canvas_set_buffer(g_canvas, s_buf[r], VW, VH, LV_IMG_CF_TRUE_COLOR);
        lv_obj_invalidate(g_canvas);
    }

    char st[128], dt[128];
    pthread_mutex_lock(&s_mtx);
    snprintf(st, sizeof(st), "%s", s_status);
    snprintf(dt, sizeof(dt), "%s", s_detail);
    int fps = s_fps;
    pthread_mutex_unlock(&s_mtx);

    if (fps > 0 && s_w > 0)
    {
        char line[160];
        snprintf(line, sizeof(line), "%s  %d fps  %dx%d%s", st, fps, s_w, s_h,
                 s_frozen ? "  [冻结]" : "");
        if (g_status != NULL && lv_obj_is_valid(g_status))
            lv_label_set_text(g_status, line);
    }
    else
    {
        if (g_status != NULL && lv_obj_is_valid(g_status))
            lv_label_set_text(g_status, st);
    }
    if (g_detail != NULL && lv_obj_is_valid(g_detail))
        lv_label_set_text(g_detail, dt);

    // 按键消费
    if (g_left > 0)
    {
        g_left--;
        dlg_move(-1);
    }
    if (g_right > 0)
    {
        g_right--;
        dlg_move(1);
    }
    if (g_enter > 0)
    {
        g_enter--;
        if (g_dialog != NULL)
            dlg_ok();
        else if (!have_ecm_iface())
            open_dialog(1); // 还没启用 USB 网卡 -> 先问
        else
            s_req_reconnect = true;
    }
    if (g_enter_long > 0)
    {
        g_enter_long--;
        if (g_dialog == NULL)
            open_dialog(2); // 长按确认 -> 恢复 adb
    }
    if (g_rec > 0)
    {
        g_rec--;
        if (g_dialog != NULL)
            close_dialog();
        else
        {
            s_frozen = !s_frozen;
            if (g_hint != NULL && lv_obj_is_valid(g_hint))
                lv_label_set_text(g_hint, s_frozen ? "已冻结  录像键继续  长按确认键恢复adb"
                                                   : "确认=重连  录像=冻结  长按确认=恢复adb  菜单=返回");
        }
    }
}

extern "C" const lios_app_info_t lios_app_info = {
    "bw21cam", "BW21摄像头", "摄", 47, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    s_frozen = false;
    s_ready = -1;
    s_front = 0;
    s_fps = 0;
    s_w = s_h = 0;

    if (s_buf[0] == NULL)
        s_buf[0] = (uint8_t *)malloc((size_t)VW * VH * 4);
    if (s_buf[1] == NULL)
        s_buf[1] = (uint8_t *)malloc((size_t)VW * VH * 4);
    if (s_buf[0] != NULL)
        memset(s_buf[0], 0, (size_t)VW * VH * 4);
    if (s_buf[1] != NULL)
        memset(s_buf[1], 0, (size_t)VW * VH * 4);

    g_canvas = lv_canvas_create(parent);
    lv_obj_set_pos(g_canvas, 0, 0);
    if (s_buf[0] != NULL)
        lv_canvas_set_buffer(g_canvas, s_buf[0], VW, VH, LV_IMG_CF_TRUE_COLOR);

    // 半透明底衬，让状态字在亮画面上也看得清
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 34);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_status = lv_label_create(parent);
    lv_obj_set_style_text_font(g_status, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_status, lv_color_hex(0x7DF0A0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(g_status, 4, 2);
    lv_label_set_text(g_status, "未连接");

    g_detail = lv_label_create(parent);
    lv_obj_set_width(g_detail, 312);
    lv_obj_set_style_text_font(g_detail, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_detail, lv_color_hex(0xFFD479), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(g_detail, 4, 18);
    lv_label_set_text(g_detail, "");

    g_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(g_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_hint, lv_color_hex(0xC8D4DE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_hint, "确认=重连  录像=冻结  长按确认=恢复adb  菜单=返回");
    lv_obj_align(g_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    if (!s_started)
    {
        s_run = true;
        if (pthread_create(&s_thread, NULL, worker, NULL) == 0)
        {
            pthread_detach(s_thread);
            s_started = true;
        }
        else
        {
            s_run = false;
            lv_label_set_text(g_status, "线程创建失败");
        }
    }

    if (g_timer == NULL)
        g_timer = lv_timer_create(ui_tick, 40, NULL);

    if (!have_ecm_iface())
        open_dialog(1); // 第一次进来先问要不要启用 USB 网卡

    return parent;
}

extern "C" void lios_app_destroy(void)
{
    s_run = false;
    // 等 worker 退出（最多 4 秒；socket 有 3 秒超时）
    for (int i = 0; i < 40 && s_started; i++)
        usleep(100 * 1000);

    if (g_timer != NULL)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
    close_dialog();
    g_root = NULL;
    g_canvas = NULL;
    g_status = g_detail = g_hint = NULL;

    if (s_buf[0] != NULL)
    {
        free(s_buf[0]);
        s_buf[0] = NULL;
    }
    if (s_buf[1] != NULL)
    {
        free(s_buf[1]);
        s_buf[1] = NULL;
    }
}

extern "C" void lios_app_key(int key, int event)
{
    if (key == LIOS_KEY_LEFT && event == LIOS_EV_PRESS)
    {
        if (g_left < 4)
            g_left++;
    }
    else if (key == LIOS_KEY_RIGHT && event == LIOS_EV_PRESS)
    {
        if (g_right < 4)
            g_right++;
    }
    else if (key == LIOS_KEY_REC && event == LIOS_EV_PRESS)
    {
        if (g_rec < 4)
            g_rec++;
    }
    else if (key == LIOS_KEY_ENTER)
    {
        if (event == LIOS_EV_PRESS)
        {
            g_enter_down = true;
            g_enter_ms = lv_tick_get();
        }
        else if (event == LIOS_EV_RELEASE)
        {
            if (g_enter_down)
            {
                bool was_long = (lv_tick_elaps(g_enter_ms) >= 1500);
                g_enter_down = false;
                if (was_long)
                    g_enter_long++;
                else if (g_enter < 4)
                    g_enter++;
            }
        }
        else if (event == LIOS_EV_LONG)
        {
            g_enter_down = false;
            g_enter_long++;
        }
    }
}
