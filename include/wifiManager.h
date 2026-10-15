#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

// 相机本机 WiFi 管理（通用的那部分）
//
// 只做一件事：驱动相机自己的无线网卡（wlan0，例如 USB 的 MT7612U / 内置模块），
// 用 wpa_supplicant + wpa_cli + udhcpc 扫描、连接、查状态。
// 网络操作都在后台线程里做，界面只读快照，不阻塞 LVGL。

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MAX_APS   24
#define WIFI_SSID_LEN  33
#define WIFI_PASS_LEN  64

typedef struct
{
    char ssid[WIFI_SSID_LEN];
    int rssi;   // dBm，越大越强；0 表示未知
    int secure; // 1 = 需要密码
} wifi_ap_t;

typedef enum
{
    WIFI_ST_IDLE = 0,
    WIFI_ST_SCANNING,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,
} wifi_conn_state_t;

typedef struct
{
    int conn_state; // wifi_conn_state_t
    int busy;       // 1 = 后台正在干活
    int ap_count;
    wifi_ap_t aps[WIFI_MAX_APS];
    int selected;
    char status[128]; // 给界面显示的一行状态
    char err[96];     // 最后一次错误
    char connected_ssid[WIFI_SSID_LEN];
    char ip[32];
    uint32_t scan_seq;   // 扫描结果更新计数
    uint32_t status_seq; // 状态更新计数
} wifi_state_t;

// 供 UI（C/C++ 均可）调用
void wifi_init(void); // 只调用一次
void wifi_copy_state(wifi_state_t *dst);
void wifi_request_scan(void);
void wifi_request_status(void);
int wifi_connect(const char *ssid, const char *pass); // 立即返回；结果看状态
void wifi_disconnect(void);
int wifi_save_credentials(const char *ssid, const char *pass);
int wifi_load_credentials(char *ssid, size_t n, char *pass, size_t m);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
