// WiFi 后端实现（相机侧）
// 详见 include/wifiManager.h 的说明。所有网络操作在后台线程执行，UI 只读快照。
#include "wifiManager.h"
#include "uiTrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "httplib.h"
#include "cJSON.h"

#define WIFI_CONF_PATH "/mnt/UDISK/wifi.conf"
#define WIFI_CTL_PATH "/mnt/UDISK/wifi_ctl.txt"
#define WIFI_CTL_DEFAULT "http://192.168.42.1:8080"
#define WIFI_LOCAL_IF "wlan0"

static wifi_state_t g_st;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker;
static int g_worker_started = 0;

static int g_cmd_scan = 0, g_cmd_status = 0, g_cmd_connect = 0, g_cmd_disconnect = 0;
static char g_new_ssid[WIFI_SSID_LEN];
static char g_new_pass[WIFI_PASS_LEN];
static char g_ctl[160] = WIFI_CTL_DEFAULT;

// ---------------------------------------------------------------- 小工具
static void st_setf(int seq, char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    if (seq)
        g_st.status_seq++;
}
#define SET_STATUS(...) st_setf(1, g_st.status, sizeof(g_st.status), __VA_ARGS__)
#define SET_ERR(...)    st_setf(0, g_st.err, sizeof(g_st.err), __VA_ARGS__)

static void str_copy(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0)
        return;
    if (!src)
        src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

// 执行命令并收集 stdout（本地后端用）
static int run_cmd(const char *cmd, char *out, size_t outn)
{
    if (out && outn)
        out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    size_t used = 0;
    if (out && outn > 1)
    {
        size_t got = fread(out, 1, outn - 1, p);
        used = got;
    }
    if (out && outn)
        out[used] = 0;
    int rc = pclose(p);
    return rc;
}

// ---------------------------------------------------------------- 后端探测
static void probe_backend(void)
{
    // 1) BW21 控制 API
    try
    {
        httplib::Client cli(g_ctl);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(3, 0);
        auto res = cli.Get("/api/sys/info");
        if (res && res->status == 200)
        {
            g_st.backend = WIFI_BACKEND_BW21;
            SET_STATUS("已连接 BW21 控制接口 %s", g_ctl);
            return;
        }
    }
    catch (...)
    {
    }
    // 2) 本机 wlan0
    if (access("/sys/class/net/" WIFI_LOCAL_IF, F_OK) == 0)
    {
        g_st.backend = WIFI_BACKEND_LOCAL;
        SET_STATUS("使用本机 %s", WIFI_LOCAL_IF);
        return;
    }
    // 3) 演示模式
    g_st.backend = WIFI_BACKEND_DEMO;
    SET_STATUS("未找到网络后端（BW21），当前为演示数据");
}

// ---------------------------------------------------------------- DEMO 数据
static void scan_demo(void)
{
    static const struct
    {
        const char *ssid;
        int rssi;
        int sec;
    } demo[] = {
        {"LiThermal-Demo", -45, 1},
        {"HomeWiFi_5G", -61, 1},
        {"TP-LINK_2.4G", -72, 1},
        {"ChinaNet-open", -80, 0},
        {"Xiaomi_AX6000", -55, 1},
    };
    int n = sizeof(demo) / sizeof(demo[0]);
    if (n > WIFI_MAX_APS)
        n = WIFI_MAX_APS;
    for (int i = 0; i < n; i++)
    {
        str_copy(g_st.aps[i].ssid, WIFI_SSID_LEN, demo[i].ssid);
        g_st.aps[i].rssi = demo[i].rssi;
        g_st.aps[i].secure = demo[i].sec;
    }
    g_st.ap_count = n;
    g_st.scan_seq++;
}

static void connect_demo(const char *ssid, const char *pass)
{
    (void)pass;
    SET_STATUS("演示：正在模拟连接 %s ...", ssid);
    g_st.conn_state = WIFI_ST_CONNECTING;
    usleep(1200 * 1000);
    str_copy(g_st.connected_ssid, WIFI_SSID_LEN, ssid);
    str_copy(g_st.ip, sizeof(g_st.ip), "192.168.42.2");
    g_st.conn_state = WIFI_ST_CONNECTED;
    SET_STATUS("演示：已模拟连接 %s（未真正联网）", ssid);
}

// ---------------------------------------------------------------- BW21 后端
static void scan_bw21(void)
{
    httplib::Client cli(g_ctl);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(6, 0);
    auto res = cli.Get("/api/wifi/scan");
    if (!res || res->status != 200)
    {
        SET_ERR("扫描失败：%s", res ? "HTTP 错误" : "连不上 BW21");
        SET_STATUS("扫描失败：BW21 无响应");
        return;
    }
    cJSON *root = cJSON_Parse(res->body.c_str());
    if (!root)
    {
        SET_ERR("扫描结果不是合法 JSON");
        SET_STATUS("扫描失败：返回格式错误");
        return;
    }
    int n = 0;
    cJSON *aps = cJSON_GetObjectItem(root, "aps");
    if (aps)
    {
        int total = cJSON_GetArraySize(aps);
        for (int i = 0; i < total && n < WIFI_MAX_APS; i++)
        {
            cJSON *it = cJSON_GetArrayItem(aps, i);
            if (!it)
                continue;
            cJSON *jssid = cJSON_GetObjectItem(it, "ssid");
            cJSON *jrssi = cJSON_GetObjectItem(it, "rssi");
            cJSON *jsec = cJSON_GetObjectItem(it, "sec");
            if (!jssid || !jssid->valuestring)
                continue;
            str_copy(g_st.aps[n].ssid, WIFI_SSID_LEN, jssid->valuestring);
            g_st.aps[n].rssi = jrssi ? jrssi->valueint : 0;
            g_st.aps[n].secure = jsec ? jsec->valueint : 1;
            n++;
        }
    }
    cJSON_Delete(root);
    g_st.ap_count = n;
    g_st.scan_seq++;
    SET_STATUS("扫描完成，共 %d 个网络", n);
}

static void connect_bw21(const char *ssid, const char *pass)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", ssid);
    cJSON_AddStringToObject(o, "pass", pass ? pass : "");
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httplib::Client cli(g_ctl);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(10, 0);
    auto res = cli.Post("/api/wifi/connect", body ? body : "{}", "application/json");
    if (body)
        free(body);
    if (res && res->status == 200)
    {
        g_st.conn_state = WIFI_ST_CONNECTING;
        str_copy(g_st.connected_ssid, WIFI_SSID_LEN, ssid);
        SET_STATUS("已把 %s 的密码交给 BW21，正在连接 ...", ssid);
    }
    else
    {
        g_st.conn_state = WIFI_ST_FAILED;
        SET_ERR("BW21 未接受连接请求");
        SET_STATUS("连接失败：BW21 无响应");
    }
}

static void status_bw21(void)
{
    httplib::Client cli(g_ctl);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(4, 0);
    auto res = cli.Get("/api/wifi/status");
    if (!res || res->status != 200)
        return;
    cJSON *root = cJSON_Parse(res->body.c_str());
    if (!root)
        return;
    cJSON *jstate = cJSON_GetObjectItem(root, "state");
    cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *jip = cJSON_GetObjectItem(root, "ip");
    const char *state = (jstate && jstate->valuestring) ? jstate->valuestring : "";
    if (strcmp(state, "connected") == 0)
    {
        g_st.conn_state = WIFI_ST_CONNECTED;
        if (jssid && jssid->valuestring)
            str_copy(g_st.connected_ssid, WIFI_SSID_LEN, jssid->valuestring);
        if (jip && jip->valuestring)
            str_copy(g_st.ip, sizeof(g_st.ip), jip->valuestring);
        SET_STATUS("已连接 %s  地址 %s", g_st.connected_ssid, g_st.ip);
    }
    else if (strcmp(state, "connecting") == 0)
    {
        g_st.conn_state = WIFI_ST_CONNECTING;
        SET_STATUS("正在连接 %s ...", (jssid && jssid->valuestring) ? jssid->valuestring : "");
    }
    else if (strcmp(state, "failed") == 0)
    {
        g_st.conn_state = WIFI_ST_FAILED;
        SET_STATUS("连接失败（密码错或信号太弱）");
    }
    else
    {
        g_st.conn_state = WIFI_ST_IDLE;
        SET_STATUS("未连接 WiFi");
    }
    cJSON_Delete(root);
}

// ---------------------------------------------------------------- 本机 wlan0 后端
static void scan_local(void)
{
    char buf[4096];
    // 先触发一次扫描，再取结果
    run_cmd("wpa_cli -i " WIFI_LOCAL_IF " scan >/dev/null 2>&1", NULL, 0);
    sleep(2);
    if (run_cmd("wpa_cli -i " WIFI_LOCAL_IF " scan_results 2>/dev/null", buf, sizeof(buf)) != 0)
    {
        SET_ERR("wpa_cli 执行失败");
        SET_STATUS("扫描失败：wpa_cli 不可用");
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
                        size_t L = strlen(ssid);
                        while (L && (ssid[L - 1] == '\r' || ssid[L - 1] == '\n'))
                            ssid[--L] = 0;
                        if (L)
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
    g_st.scan_seq++;
    SET_STATUS("扫描完成，共 %d 个网络", n);
}

static void connect_local(const char *ssid, const char *pass)
{
    char cmd[512];
    FILE *f = fopen("/etc/wpa_supplicant.conf", "w");
    if (!f)
    {
        g_st.conn_state = WIFI_ST_FAILED;
        SET_ERR("无法写 wpa 配置文件");
        SET_STATUS("连接失败：配置不可写");
        return;
    }
    fprintf(f, "ctrl_interface=/var/log/wpa_supplicant\nupdate_config=1\n");
    fprintf(f, "network={\n\tssid=\"%s\"\n", ssid);
    if (pass && pass[0])
        fprintf(f, "\tpsk=\"%s\"\n", pass);
    else
        fprintf(f, "\tkey_mgmt=NONE\n");
    fprintf(f, "}\n");
    fclose(f);

    snprintf(cmd, sizeof(cmd), "wpa_cli -i " WIFI_LOCAL_IF " reconfigure >/dev/null 2>&1 || killall -HUP wpa_supplicant");
    run_cmd(cmd, NULL, 0);
    g_st.conn_state = WIFI_ST_CONNECTING;
    str_copy(g_st.connected_ssid, WIFI_SSID_LEN, ssid);
    SET_STATUS("正在连接 %s ...", ssid);
    sleep(6);
    snprintf(cmd, sizeof(cmd), "udhcpc -i " WIFI_LOCAL_IF " -n -q -t 5 -T 3 >/dev/null 2>&1");
    run_cmd(cmd, NULL, 0);
}

static void status_local(void)
{
    char buf[1024];
    if (run_cmd("wpa_cli -i " WIFI_LOCAL_IF " status 2>/dev/null", buf, sizeof(buf)) != 0)
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
            str_copy(g_st.connected_ssid, WIFI_SSID_LEN, p);
        }
        char ip[64] = "";
        if (run_cmd("ip -4 -o addr show " WIFI_LOCAL_IF " 2>/dev/null | awk '{print $4}' | cut -d/ -f1", ip, sizeof(ip)) == 0)
        {
            size_t L = strlen(ip);
            while (L && (ip[L - 1] == '\n' || ip[L - 1] == '\r'))
                ip[--L] = 0;
            str_copy(g_st.ip, sizeof(g_st.ip), ip);
        }
        SET_STATUS("已连接 %s  地址 %s", g_st.connected_ssid, g_st.ip);
    }
    else
    {
        g_st.conn_state = WIFI_ST_IDLE;
        SET_STATUS("未连接 WiFi");
    }
}

// ---------------------------------------------------------------- 任务派发
static void do_scan(void)
{
    g_st.conn_state = (g_st.conn_state == WIFI_ST_CONNECTING) ? g_st.conn_state : WIFI_ST_SCANNING;
    switch (g_st.backend)
    {
    case WIFI_BACKEND_BW21:
        scan_bw21();
        break;
    case WIFI_BACKEND_LOCAL:
        scan_local();
        break;
    default:
        scan_demo();
        break;
    }
}

static void do_connect(const char *ssid, const char *pass)
{
    switch (g_st.backend)
    {
    case WIFI_BACKEND_BW21:
        connect_bw21(ssid, pass);
        break;
    case WIFI_BACKEND_LOCAL:
        connect_local(ssid, pass);
        break;
    default:
        connect_demo(ssid, pass);
        break;
    }
}

static void do_status(void)
{
    if (g_st.backend == WIFI_BACKEND_BW21)
        status_bw21();
    else if (g_st.backend == WIFI_BACKEND_LOCAL)
        status_local();
}

static void *wifi_worker(void *)
{
    ui_trace("wifiWorker: 线程启动，探测后端");
    probe_backend();
    ui_trace("wifiWorker: 后端=%d", (int)g_st.backend);
    if (g_st.backend != WIFI_BACKEND_DEMO)
    {
        do_scan();
        do_status();
    }
    else
    {
        scan_demo();
    }
    ui_trace("wifiWorker: 首次探测完成，进入等待");
    while (1)
    {
        pthread_mutex_lock(&g_lock);
        while (!g_cmd_scan && !g_cmd_status && !g_cmd_connect && !g_cmd_disconnect)
            pthread_cond_wait(&g_cv, &g_lock);
        int sc = g_cmd_scan, stt = g_cmd_status, cn = g_cmd_connect, dc = g_cmd_disconnect;
        char ssid[WIFI_SSID_LEN], pass[WIFI_PASS_LEN];
        str_copy(ssid, sizeof(ssid), g_new_ssid);
        str_copy(pass, sizeof(pass), g_new_pass);
        g_cmd_scan = g_cmd_status = g_cmd_connect = g_cmd_disconnect = 0;
        g_st.busy = 1;
        pthread_mutex_unlock(&g_lock);

        ui_trace("wifiWorker: 收到命令 scan=%d status=%d connect=%d disc=%d", sc, stt, cn, dc);
        if (cn)
            do_connect(ssid, pass);
        if (dc)
        {
            g_st.conn_state = WIFI_ST_IDLE;
            SET_STATUS("已断开");
        }
        if (sc)
            do_scan();
        if (stt)
            do_status();
        ui_trace("wifiWorker: 命令处理完成");

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
extern "C" void wifi_init(const char *ctl_base)
{
    if (ctl_base && ctl_base[0])
        str_copy(g_ctl, sizeof(g_ctl), ctl_base);
    else
    {
        // 允许用文件覆盖控制接口地址（不用重新编译固件）
        FILE *f = fopen(WIFI_CTL_PATH, "r");
        if (f)
        {
            char line[160] = "";
            if (fgets(line, sizeof(line), f))
            {
                size_t L = strlen(line);
                while (L && (line[L - 1] == '\n' || line[L - 1] == '\r' || line[L - 1] == ' '))
                    line[--L] = 0;
                if (L > 7)
                    str_copy(g_ctl, sizeof(g_ctl), line);
            }
            fclose(f);
        }
    }
    memset(&g_st, 0, sizeof(g_st));
    g_st.backend = WIFI_BACKEND_NONE;
    g_st.conn_state = WIFI_ST_IDLE;
    str_copy(g_st.status, sizeof(g_st.status), "正在检测网络后端 ...");
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

extern "C" const char *wifi_control_base(void)
{
    return g_ctl;
}

extern "C" const char *wifi_backend_name(int backend)
{
    switch (backend)
    {
    case WIFI_BACKEND_BW21:
        return "BW21";
    case WIFI_BACKEND_LOCAL:
        return "本机 WiFi";
    case WIFI_BACKEND_DEMO:
        return "演示";
    default:
        return "无";
    }
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
    // 连接成功后再请求一次状态，让界面能看到 IP
    g_cmd_status = 1;
    pthread_mutex_unlock(&g_lock);
    kick();
    wifi_save_credentials(ssid, pass ? pass : "");
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
    fprintf(f, "SSID=%s\nPSK=%s\n", ssid ? ssid : "", pass ? pass : "");
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
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = 0;
        if (strncmp(line, "SSID=", 5) == 0 && ssid)
            str_copy(ssid, n, line + 5);
        else if (strncmp(line, "PSK=", 4) == 0 && pass)
            str_copy(pass, m, line + 4);
    }
    fclose(f);
    return 0;
}

/* ================================================================== 网络功能总开关
 * 默认关闭：/mnt/UDISK/wifi_enabled 里是 1 才算启用。
 * 关着的时候开机脚本不碰 USB（保持原生 adb+MTP），相机功能完全正常。
 * 开启时顺手把 usbnet.conf 的 MODE 改成 ecm（BW21 的驱动只认 ECM），并写好控制接口地址。
 */
#define NET_FLAG_PATH "/mnt/UDISK/wifi_enabled"
#define USB_CONF_PATH "/mnt/UDISK/usbnet.conf"
#define NET_CTL_URL   "http://192.168.2.250:8080"

extern "C" int net_feature_enabled(void)
{
	FILE *f = fopen(NET_FLAG_PATH, "r");
	char b[8] = {0};
	if (f == NULL) {
		return 0; /* 没有标记文件 = 默认关闭 */
	}
	if (fgets(b, sizeof(b), f) == NULL) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return (b[0] == '1') ? 1 : 0;
}

/* 把 usbnet.conf 里的 KEY=... 改成 KEY=val（没有这一行就追加） */
static void net_conf_set_kv(const char *key, const char *val)
{
	char buf[4096];
	char want[64];
	size_t n = 0;
	size_t klen = strlen(key);
	FILE *f = fopen(USB_CONF_PATH, "r");
	FILE *o;
	char *p;
	int wrote = 0;

	snprintf(want, sizeof(want), "%s=%s", key, val);

	if (f != NULL) {
		n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
	}
	buf[n] = 0;

	o = fopen(USB_CONF_PATH, "w");
	if (o == NULL) {
		return;
	}
	p = buf;
	while (p != NULL && *p != 0) {
		char *nl = strchr(p, '\n');
		size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
		if (len >= klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=' && !wrote) {
			fprintf(o, "%s\n", want);
			wrote = 1;
		} else if (len > 0) {
			fwrite(p, 1, len, o);
			fputc('\n', o);
		}
		p = (nl != NULL) ? (nl + 1) : NULL;
	}
	if (!wrote) {
		fprintf(o, "%s\n", want);
	}
	fclose(o);
}

extern "C" void net_feature_set(int enable)
{
	FILE *f = fopen(NET_FLAG_PATH, "w");
	if (f != NULL) {
		fprintf(f, "%d\n", enable ? 1 : 0);
		fclose(f);
	}
	/* 开 = 出 ECM 网卡；关 = 不出网卡，USB 口回到原生 adb+MTP */
	net_conf_set_kv("MODE", enable ? "ecm" : "off");

	if (enable) {
		/* 相机作为家里 LAN 的一员（BW21 做代理 ARP + IP 转发）：
		   IP 用 BW21 网段的静态地址，网关指向 BW21，DNS 直接用路由器 */
		net_conf_set_kv("USE_DHCP", "0");
		net_conf_set_kv("IP", "192.168.2.240");
		net_conf_set_kv("MASK", "255.255.255.0");
		net_conf_set_kv("GW", "192.168.2.250");
		net_conf_set_kv("DNS1", "192.168.2.1");
	}

	/* 控制接口地址（相机界面用它指挥 BW21） */
	f = fopen(WIFI_CTL_PATH, "w");
	if (f != NULL) {
		fprintf(f, "%s\n", NET_CTL_URL);
		fclose(f);
	}
}
