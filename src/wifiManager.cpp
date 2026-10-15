// 相机本机 WiFi 管理（通用版）
//
// 驱动相机自己的无线网卡（wlan0，例如 USB 的 MT7612U），走
// wpa_supplicant + wpa_cli + udhcpc。所有操作在后台线程完成，
// 界面只读状态快照（wifi_state_t），不会阻塞 LVGL。

#include "wifiManager.h"
#include "uiTrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#define WIFI_CONF_PATH "/mnt/UDISK/wifi.conf"          // SSID=/PSK= 已保存凭据
#define WPA_CONF_PATH  "/mnt/UDISK/wpa_supplicant.conf" // 生成的 wpa_supplicant 配置
#define WPA_CTRL_DIR   "/var/run/wpa_supplicant"       // ctrl_interface 目录

static wifi_state_t g_st;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker;
static int g_worker_started = 0;
static int g_auto_connect_done = 0;

static int g_cmd_scan = 0, g_cmd_status = 0, g_cmd_connect = 0, g_cmd_disconnect = 0;
static char g_new_ssid[WIFI_SSID_LEN];
static char g_new_pass[WIFI_PASS_LEN];

static char g_if[16] = "wlan0"; // 实际使用的无线网卡
static int g_wpa_started = 0;   // 我们自己拉起过 wpa_supplicant

// ---------------------------------------------------------------- 小工具
static void st_setf(int seq, char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    char tmp[sizeof(g_st.status)];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    snprintf(dst, n, "%s", tmp);
    g_st.status_seq++;
    (void)seq;
    pthread_mutex_unlock(&g_lock);
}

#define SET_STATUS(...) st_setf(0, g_st.status, sizeof(g_st.status), __VA_ARGS__)
#define SET_ERR(...)    st_setf(0, g_st.err, sizeof(g_st.err), __VA_ARGS__)

static void str_copy(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, n, "%s", src);
}

static void trim_nl(char *s)
{
    if (!s)
        return;
    size_t L = strlen(s);
    while (L && (s[L - 1] == '\n' || s[L - 1] == '\r' || s[L - 1] == ' ' || s[L - 1] == '\t'))
        s[--L] = 0;
}

// 跑一条命令，可选拿输出
static int run_cmd(const char *cmd, char *out, size_t outn)
{
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    if (out && outn)
    {
        size_t got = fread(out, 1, outn - 1, p);
        out[got] = 0;
    }
    int rc = pclose(p);
    return rc;
}

// ---------------------------------------------------------------- 网卡 / wpa
static int iface_exists(const char *n)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s", n);
    return access(path, F_OK) == 0;
}

static void pick_iface(void)
{
    // 优先用已记录的；否则在 /sys/class/net 里找第一个 wlan*
    if (iface_exists(g_if))
        return;
    char buf[2048];
    if (run_cmd("ls /sys/class/net 2>/dev/null", buf, sizeof(buf)) != 0)
        return;
    char *line = strtok(buf, "\n");
    while (line)
    {
        trim_nl(line);
        if (strncmp(line, "wlan", 4) == 0)
        {
            str_copy(g_if, sizeof(g_if), line);
            ui_trace("wifi: 使用无线网卡 %s", g_if);
            return;
        }
        line = strtok(NULL, "\n");
    }
}

static void ensure_wpa(void)
{
    char cmd[256];
    // 网卡先 up
    snprintf(cmd, sizeof(cmd), "ifconfig %s up 2>/dev/null", g_if);
    run_cmd(cmd, NULL, 0);

    // wpa_supplicant 没跑就自己拉起来（-B 后台）
    if (run_cmd("ps | grep -v grep | grep -q wpa_supplicant", NULL, 0) != 0)
    {
        mkdir(WPA_CTRL_DIR, 0777);
        if (access(WPA_CONF_PATH, F_OK) != 0)
        {
            FILE *f = fopen(WPA_CONF_PATH, "w");
            if (f)
            {
                fprintf(f, "ctrl_interface=%s\nupdate_config=1\n", WPA_CTRL_DIR);
                fclose(f);
            }
        }
        snprintf(cmd, sizeof(cmd), "wpa_supplicant -B -i %s -c %s -D nl80211 2>/dev/null", g_if, WPA_CONF_PATH);
        if (run_cmd(cmd, NULL, 0) != 0)
        {
            snprintf(cmd, sizeof(cmd), "wpa_supplicant -B -i %s -c %s 2>/dev/null", g_if, WPA_CONF_PATH);
            run_cmd(cmd, NULL, 0);
        }
        g_wpa_started = 1;
        ui_trace("wifi: 已启动 wpa_supplicant (%s)", g_if);
        sleep(2);
    }
}

static int wpa_cli(const char *args, char *out, size_t outn)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "wpa_cli -i %s -p %s %s 2>/dev/null", g_if, WPA_CTRL_DIR, args);
    return run_cmd(cmd, out, outn);
}

// ---------------------------------------------------------------- 扫描 / 连接 / 状态
static void scan_local(void)
{
    char buf[4096];
    SET_STATUS("正在扫描 ...");
    g_st.conn_state = WIFI_ST_SCANNING;
    g_st.status_seq++;

    wpa_cli("scan >/dev/null", NULL, 0);
    sleep(2);

    if (wpa_cli("scan_results", buf, sizeof(buf)) != 0)
    {
        SET_ERR("wpa_cli 执行失败");
        SET_STATUS("扫描失败：wpa_cli 不可用（无线网卡没起来？）");
        g_st.conn_state = WIFI_ST_FAILED;
        g_st.status_seq++;
        return;
    }

    int n = 0;
    char *line = strtok(buf, "\n");
    if (line && strstr(line, "bssid"))
        line = strtok(NULL, "\n"); // 跳过表头
    while (line && n < WIFI_MAX_APS)
    {
        // 格式：bssid \t frequency \t signal \t flags \t ssid
        char *f1 = strchr(line, '\t');
        if (f1)
        {
            char *f2 = strchr(f1 + 1, '\t');
            if (f2)
            {
                char *f3 = strchr(f2 + 1, '\t');
                if (f3)
                {
                    char *ssid = strchr(f3 + 1, '\t');
                    if (ssid)
                    {
                        int rssi = atoi(f2 + 1);
                        int sec = (strstr(f3 + 1, "WPA") || strstr(f3 + 1, "WEP")) ? 1 : 0;
                        ssid++;
                        trim_nl(ssid);
                        if (ssid[0])
                        {
                            str_copy(g_st.aps[n].ssid, WIFI_SSID_LEN, ssid);
                            g_st.aps[n].rssi = rssi;
                            g_st.aps[n].secure = sec;
                            n++;
                        }
                    }
                }
            }
        }
        line = strtok(NULL, "\n");
    }

    g_st.ap_count = n;
    pthread_mutex_lock(&g_lock);
    g_st.scan_seq++;
    pthread_mutex_unlock(&g_lock);
    g_st.conn_state = WIFI_ST_IDLE;
    g_st.status_seq++;
    if (n == 0)
        SET_STATUS("没扫到网络（无线网卡正常吗？可重试一次）");
    else
        SET_STATUS("扫描完成，共 %d 个网络", n);
}

static int write_wpa_network(const char *ssid, const char *pass)
{
    FILE *f = fopen(WPA_CONF_PATH, "w");
    if (!f)
        return -1;
    fprintf(f, "ctrl_interface=%s\nupdate_config=1\n", WPA_CTRL_DIR);
    fprintf(f, "network={\n\tssid=\"%s\"\n", ssid);
    if (pass && pass[0])
        fprintf(f, "\tpsk=\"%s\"\n", pass);
    else
        fprintf(f, "\tkey_mgmt=NONE\n");
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

static void connect_local(const char *ssid, const char *pass)
{
    char cmd[256];

    if (write_wpa_network(ssid, pass) != 0)
    {
        g_st.conn_state = WIFI_ST_FAILED;
        SET_ERR("无法写 %s", WPA_CONF_PATH);
        SET_STATUS("连接失败：配置写不进去");
        g_st.status_seq++;
        return;
    }

    SET_STATUS("正在连接 %s ...", ssid);
    g_st.conn_state = WIFI_ST_CONNECTING;
    str_copy(g_st.connected_ssid, WIFI_SSID_LEN, ssid);
    g_st.status_seq++;

    // 让 wpa_supplicant 重新读配置
    if (wpa_cli("reconfigure >/dev/null", NULL, 0) != 0)
    {
        snprintf(cmd, sizeof(cmd), "killall -HUP wpa_supplicant 2>/dev/null");
        run_cmd(cmd, NULL, 0);
    }
    sleep(6);

    snprintf(cmd, sizeof(cmd), "udhcpc -i %s -n -q -t 6 -T 3 >/dev/null 2>&1", g_if);
    run_cmd(cmd, NULL, 0);

    status_local();
}

static void status_local(void)
{
    char buf[1024];
    if (wpa_cli("status", buf, sizeof(buf)) != 0)
        return;

    if (strstr(buf, "wpa_state=COMPLETED"))
    {
        g_st.conn_state = WIFI_ST_CONNECTED;
        char *p = strstr(buf, "ssid=");
        if (p)
        {
            p += 5;
            char *e = strchr(p, '\n');
            if (e)
                *e = 0;
            trim_nl(p);
            str_copy(g_st.connected_ssid, WIFI_SSID_LEN, p);
        }
        char ip[64] = "";
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "ip -4 -o addr show %s 2>/dev/null | awk '{print $4}' | cut -d/ -f1", g_if);
        if (run_cmd(cmd, ip, sizeof(ip)) == 0)
        {
            trim_nl(ip);
            str_copy(g_st.ip, sizeof(g_st.ip), ip);
        }
        g_st.status_seq++;
        if (g_st.ip[0])
            SET_STATUS("已连接 %s   地址 %s", g_st.connected_ssid, g_st.ip);
        else
            SET_STATUS("已连上 %s，但还没拿到 IP", g_st.connected_ssid);
    }
    else if (strstr(buf, "wpa_state=SCANNING") || strstr(buf, "wpa_state=ASSOCIATING") ||
             strstr(buf, "wpa_state=4WAY_HANDSHAKE"))
    {
        g_st.conn_state = WIFI_ST_CONNECTING;
        g_st.status_seq++;
        SET_STATUS("正在连接 %s ...", g_st.connected_ssid);
    }
    else
    {
        g_st.conn_state = WIFI_ST_IDLE;
        g_st.status_seq++;
        SET_STATUS("未连接 WiFi");
    }
}

// ---------------------------------------------------------------- 任务派发
static void do_scan(void)
{
    if (!iface_exists(g_if))
    {
        pick_iface();
        if (!iface_exists(g_if))
        {
            g_st.conn_state = WIFI_ST_FAILED;
            g_st.status_seq++;
            SET_STATUS("未找到无线网卡（wlan0）。\n请确认 USB 无线网卡已插好、驱动已加载。");
            return;
        }
    }
    ensure_wpa();
    scan_local();
}

static void do_connect(const char *ssid, const char *pass)
{
    if (!iface_exists(g_if))
        pick_iface();
    if (!iface_exists(g_if))
    {
        g_st.conn_state = WIFI_ST_FAILED;
        g_st.status_seq++;
        SET_STATUS("未找到无线网卡，无法连接");
        return;
    }
    ensure_wpa();
    connect_local(ssid, pass);
}

static void do_status(void)
{
    if (iface_exists(g_if))
    {
        ensure_wpa();
        status_local();
    }
}

static void *wifi_worker(void *)
{
    ui_trace("wifiWorker: 启动");
    pick_iface();
    if (iface_exists(g_if))
    {
        ensure_wpa();
        status_local();
    }
    else
    {
        SET_STATUS("未找到无线网卡（wlan0）。\n请确认 USB 无线网卡已插好、驱动已加载。");
    }
    ui_trace("wifiWorker: 就绪，iface=%s exists=%d", g_if, iface_exists(g_if));

    int idle_ticks = 0;
    while (1)
    {
        pthread_mutex_lock(&g_lock);
        if (!g_cmd_scan && !g_cmd_status && !g_cmd_connect && !g_cmd_disconnect)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 3; // 每 3 秒醒一次：网卡/驱动可能是后插上的
            pthread_cond_timedwait(&g_cv, &g_lock, &ts);
        }
        int sc = g_cmd_scan, stt = g_cmd_status, cn = g_cmd_connect, dc = g_cmd_disconnect;
        char ssid[WIFI_SSID_LEN], pass[WIFI_PASS_LEN];
        str_copy(ssid, sizeof(ssid), g_new_ssid);
        str_copy(pass, sizeof(pass), g_new_pass);
        g_cmd_scan = g_cmd_status = g_cmd_connect = g_cmd_disconnect = 0;
        g_st.busy = 1;
        pthread_mutex_unlock(&g_lock);

        // 网卡是后插的（比如 USB 网卡刚插上 / 模块刚加载）
        if (!iface_exists(g_if))
        {
            pick_iface();
            if (iface_exists(g_if))
            {
                ui_trace("wifiWorker: 检测到网卡 %s，初始化", g_if);
                g_auto_connect_done = 0;
                ensure_wpa();
                do_status();
            }
        }

        // 开机自动用已保存的凭据连一次
        if (iface_exists(g_if) && !g_auto_connect_done)
        {
            char a_ssid[WIFI_SSID_LEN], a_pass[WIFI_PASS_LEN];
            g_auto_connect_done = 1;
            if (wifi_load_credentials(a_ssid, sizeof(a_ssid), a_pass, sizeof(a_pass)) == 0 && a_ssid[0])
            {
                ui_trace("wifiWorker: 自动连接 %s", a_ssid);
                do_connect(a_ssid, a_pass);
            }
        }

        if (sc)
            do_scan();
        if (stt)
            do_status();
        if (cn)
            do_connect(ssid, pass);
        if (dc)
        {
            wpa_cli("disconnect >/dev/null", NULL, 0);
            g_st.conn_state = WIFI_ST_IDLE;
            g_st.status_seq++;
            SET_STATUS("已断开");
        }

        if (!sc && !stt && !cn && !dc && iface_exists(g_if))
        {
            if (++idle_ticks >= 2)
            {
                idle_ticks = 0;
                do_status();
            }
        }

        pthread_mutex_lock(&g_lock);
        g_st.busy = 0;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void kick(void)
{
    pthread_mutex_lock(&g_lock);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- 对外接口
extern "C" void wifi_init(void)
{
    pthread_mutex_lock(&g_lock);
    memset(&g_st, 0, sizeof(g_st));
    g_st.conn_state = WIFI_ST_IDLE;
    snprintf(g_st.status, sizeof(g_st.status), "正在检测无线网卡 ...");
    pthread_mutex_unlock(&g_lock);

    if (!g_worker_started)
    {
        g_worker_started = 1;
        pthread_create(&g_worker, NULL, wifi_worker, NULL);
    }
}

extern "C" void wifi_copy_state(wifi_state_t *dst)
{
    if (!dst)
        return;
    pthread_mutex_lock(&g_lock);
    *dst = g_st;
    pthread_mutex_unlock(&g_lock);
}

extern "C" void wifi_request_scan(void)
{
    pthread_mutex_lock(&g_lock);
    g_cmd_scan = 1;
    pthread_mutex_unlock(&g_lock);
    kick();
}

extern "C" void wifi_request_status(void)
{
    pthread_mutex_lock(&g_lock);
    g_cmd_status = 1;
    pthread_mutex_unlock(&g_lock);
    kick();
}

extern "C" int wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0])
        return -1;
    pthread_mutex_lock(&g_lock);
    str_copy(g_new_ssid, sizeof(g_new_ssid), ssid);
    str_copy(g_new_pass, sizeof(g_new_pass), pass ? pass : "");
    g_cmd_connect = 1;
    pthread_mutex_unlock(&g_lock);
    kick();

    // 顺手把凭据存下来，下次开机自动连
    if (wifi_save_credentials(ssid, pass ? pass : "") != 0)
        ui_trace("wifi: 凭据保存失败");
    return 0;
}

extern "C" void wifi_disconnect(void)
{
    pthread_mutex_lock(&g_lock);
    g_cmd_disconnect = 1;
    pthread_mutex_unlock(&g_lock);
    kick();
}

extern "C" int wifi_save_credentials(const char *ssid, const char *pass)
{
    FILE *f = fopen(WIFI_CONF_PATH, "w");
    if (!f)
        return -1;
    fprintf(f, "SSID=%s\n", ssid ? ssid : "");
    fprintf(f, "PSK=%s\n", pass ? pass : "");
    fclose(f);
    return 0;
}

extern "C" int wifi_load_credentials(char *ssid, size_t n, char *pass, size_t m)
{
    if (ssid && n)
        ssid[0] = 0;
    if (pass && m)
        pass[0] = 0;

    FILE *f = fopen(WIFI_CONF_PATH, "r");
    if (!f)
        return -1;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        trim_nl(line);
        if (strncmp(line, "SSID=", 5) == 0 && ssid)
            str_copy(ssid, n, line + 5);
        else if (strncmp(line, "PSK=", 4) == 0 && pass)
            str_copy(pass, m, line + 4);
    }
    fclose(f);
    return 0;
}
