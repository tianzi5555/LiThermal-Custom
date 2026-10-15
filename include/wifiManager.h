#pragma once
// WiFi 管理后端（相机侧）
//
// 设计：相机的无线网卡由外部提供，两种可能的后端：
//   1) BW21（USB 网卡方案）—— 通过 USB 链路访问 BW21 上的控制 API（HTTP/JSON）
//        默认 http://192.168.42.1:8080 ，可用 /mnt/UDISK/wifi_ctl.txt 覆盖（第一行写 base URL）
//        API：
//          GET  /api/sys/info            -> {"model":"LiThermal-BW21","fw":"..."}
//          GET  /api/wifi/scan           -> {"ok":1,"aps":[{"ssid":"x","rssi":-52,"sec":1}]}
//          POST /api/wifi/connect        <- {"ssid":"x","pass":"y"}  -> {"ok":1}
//          POST /api/wifi/disconnect     -> {"ok":1}
//          GET  /api/wifi/status         -> {"state":"connected","ssid":"x","ip":"192.168.2.201"}
//   2) 本机 wlan0（插了 USB WiFi 网卡时才有）+ wpa_supplicant/wpa_cli/udhcpc
// 都没有时进入 DEMO 后端：给出示例列表，方便在没有硬件时先验证界面与键盘。
//
// 所有网络操作都在后台线程里做，UI 线程只读快照，绝不阻塞界面。

#include <stdint.h>
#include <stddef.h>
#include <string.h>

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
    WIFI_BACKEND_NONE = 0,
    WIFI_BACKEND_BW21 = 1,  // 通过 USB 链路问 BW21
    WIFI_BACKEND_LOCAL = 2, // 相机本机 wlan0
    WIFI_BACKEND_DEMO = 3,  // 无硬件，示例数据
} wifi_backend_t;

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
    int backend;    // wifi_backend_t
    int conn_state; // wifi_conn_state_t
    int busy;       // 1 = 后台正在干活
    int ap_count;
    wifi_ap_t aps[WIFI_MAX_APS];
    int selected;
    char status[96]; // 给界面显示的一行状态
    char err[96];    // 最后一次错误
    char connected_ssid[WIFI_SSID_LEN];
    char ip[32];
    uint32_t scan_seq;   // 扫描结果更新计数
    uint32_t status_seq; // 状态更新计数
} wifi_state_t;

// 供 UI（C/C++ 均可）调用
void wifi_init(const char *ctl_base); // 只调用一次；ctl_base 传 NULL 用默认
void wifi_copy_state(wifi_state_t *dst);
const char *wifi_backend_name(int backend);
void wifi_request_scan(void);
void wifi_request_status(void);
int wifi_connect(const char *ssid, const char *pass); // 立即返回；结果看状态
void wifi_disconnect(void);
int wifi_save_credentials(const char *ssid, const char *pass);
int wifi_load_credentials(char *ssid, size_t n, char *pass, size_t m);
const char *wifi_control_base(void);

// ---- 网络功能总开关（隐藏页里控制） ----
// 默认关闭：关着时完全不碰 USB（保持原生 adb+MTP），相机功能正常。
// 开启时把 /mnt/UDISK/usbnet.conf 的 MODE 改成 ecm、写好控制接口地址，重启后生效。
int net_feature_enabled(void);
void net_feature_set(int enable);

#ifdef __cplusplus
}
#endif
