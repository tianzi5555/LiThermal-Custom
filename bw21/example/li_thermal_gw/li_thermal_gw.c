/*
 * LiThermal BW21 网关固件（RTL8735B / AmebaPro2）
 *
 * 作用：把 BW21 变成相机（全志 T113）的「USB 网卡 + 路由器」：
 *
 *   [相机] ──USB-C──> [BW21: USB Host + CDC-ECM]  ──代理ARP + IP转发──>  [WiFi STA] ──> 家里路由器 ──> 公网
 *
 * 关键设计（为什么不用 NAT）：本 SDK 没有 NAT/NAPT。所以让相机直接用**家里网段的地址**
 * （由 BW21 的 DHCP 服务器分配，例如 192.168.2.240），BW21：
 *   1. LAN 侧（ECM 口）对所有 ARP 请求一律代答 —— 相机把整个 /24 当直连，必须先到我们这里；
 *   2. WAN 侧（WiFi 口）对「相机地址段」的 ARP 请求代答 —— 路由器/电脑找相机时先到我们这里；
 *   3. 打开 IP_FORWARD，按目的地址转发（相机地址段 → ECM 口，其余 → WiFi 口/默认路由）。
 * 这样相机在局域网里是一等公民（电脑能直接 ping/访问），出网靠家里路由器 NAT，BW21 不需要 NAT。
 *
 * 控制 API（相机界面的「WiFi 设置」页通过 USB 链路调用，默认 http://<ECM_IP>:8080）：
 *   GET  /api/sys/info            -> {"model":"LiThermal-BW21","mode":"proxy-arp",...}
 *   GET  /api/wifi/scan           -> {"ok":1,"aps":[{"ssid":"x","rssi":-52,"sec":1},...]}
 *   POST /api/wifi/connect        <- {"ssid":"x","pass":"y"} -> {"ok":1}
 *   GET  /api/wifi/status         -> {"state":"connected","ssid":"x","ip":"192.168.2.98"}
 *   POST /api/wifi/disconnect     -> {"ok":1}
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <lwip/netifapi.h>
#include <lwip/ip_addr.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/def.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include "lwip_netconf.h"
#include "lwip_intf.h"
#include "usbh_cdc_ecm.h"
#include "dhcps.h"
#include "wifi_conf.h"

#include "FreeRTOS.h"
#include "task.h"

#include "example_li_thermal_gw.h"

/* ------------------------------------------------------------------ 基本定义 */
extern struct netif xnetif[NET_IF_NUM];

#define WLAN_IDX      0
#define ETHERNET_IDX  (NET_IF_NUM - 1)

#define GW_CTRL_PORT      8080
#define GW_MAX_AP         24
#define GW_SSID_LEN       33
#define GW_PASS_LEN       65
#define GW_ECM_HOST_OCTET 250 /* ECM 口用网段的 .250 */
#define GW_POOL_FIRST     240 /* 相机地址池 .240 ~ .247 */
#define GW_POOL_LAST      247

#define CONFIG_USBH_CDC_ECM_HOTPLUG 1

#define ECM_INIT_THREAD_STACK_SIZE    1280U
#define ECM_INIT_THREAD_PRIORITY      (tskIDLE_PRIORITY + 5)
#define ECM_LINK_THREAD_STACK_SIZE    1600U
#define ECM_LINK_THREAD_PRIORITY      (tskIDLE_PRIORITY + 3)
#define ECM_HOTPLUG_THREAD_STACK_SIZE 1024U
#define ECM_HOTPLUG_THREAD_PRIORITY   (tskIDLE_PRIORITY + 6)

#define GW_WIFI_THREAD_STACK 4096U
#define GW_HTTP_THREAD_STACK 3072U
#define GW_THREAD_PRIORITY   (tskIDLE_PRIORITY + 4)

/* ------------------------------------------------------------------ 全局状态 */
typedef struct {
	char ssid[GW_SSID_LEN];
	int rssi;
	int sec;
} gw_ap_t;

static gw_ap_t s_aps[GW_MAX_AP];
static volatile int s_ap_count = 0;
static volatile int s_scan_done = 0;
static volatile int s_scan_running = 0;

static char s_ssid[GW_SSID_LEN] = {0};
static char s_pass[GW_PASS_LEN] = {0};
static volatile int s_want_connect = 0;
static volatile int s_want_disconnect = 0;
static volatile int s_wifi_state = GW_STATE_IDLE; /* 0 idle 1 connecting 2 connected 3 failed */
static char s_wifi_ip[20] = {0};

static volatile int s_lan_ready = 0;
static volatile u32_t s_pool_first = 0; /* 主机序 */
static volatile u32_t s_pool_last = 0;  /* 主机序 */
static u32_t s_ecm_ip_nbo = 0;

static u8 *usbh_ecm_rx_buf;
static u8 usbh_ecm_tx_buf[1536] __attribute__((aligned(CACHE_LINE_SIZE)));

#if CONFIG_USBH_CDC_ECM_HOTPLUG
static u8 cdc_ecm_detach_pending;
static SemaphoreHandle_t cdc_ecm_detach_sema;
#endif

/* ------------------------------------------------------------------ 日志 */
#define GW_LOG(fmt, ...) printf("[LiGW] " fmt "\n", ##__VA_ARGS__)

/* ================================================================== 代理 ARP / 主机路由
 * 这两个函数被打了补丁的 lwIP 调用（见 bw21/tools/apply_patches.py）：
 *   etharp.c : etharp_input() 里问 li_gw_proxy_arp()
 *   ip4.c    : ip4_route()    里问 li_gw_host_route()
 * 只有在定义了 LI_THERMAL_GW 时才编进 lwIP（由本目录的 cmake 全局定义）。
 */

static int in_camera_pool(u32_t nbo_addr)
{
	u32_t h = ntohl(nbo_addr);
	if (s_pool_first == 0) {
		return 0;
	}
	return (h >= s_pool_first && h <= s_pool_last);
}

int li_gw_proxy_arp(struct netif *netif, const ip4_addr_t *target)
{
	u32_t t = target->addr;

	if (t == 0 || t == 0xFFFFFFFFu) {
		return 0; /* 0.0.0.0 / 广播：不管 */
	}
	if (ip4_addr_ismulticast(target)) {
		return 0;
	}
	if (netif == &xnetif[ETHERNET_IDX]) {
		/* LAN 侧（相机）：相机把整个 /24 当直连，任何单播我们都代答，让包先到我们这里 */
		return 1;
	}
	if (netif == &xnetif[WLAN_IDX]) {
		/* WAN 侧（WiFi）：只替相机地址池代答，别乱答别人的地址 */
		return in_camera_pool(t);
	}
	return 0;
}

struct netif *li_gw_host_route(const ip4_addr_t *dest)
{
	if (!s_lan_ready) {
		return NULL;
	}
	if (in_camera_pool(dest->addr)) {
		return &xnetif[ETHERNET_IDX];
	}
	return NULL;
}

/* ================================================================== USB Host + CDC-ECM */
static int cdc_ecm_cb_init(void) { GW_LOG("USB ECM init"); return HAL_OK; }
static int cdc_ecm_cb_deinit(void) { GW_LOG("USB ECM deinit"); return HAL_OK; }
static int cdc_ecm_cb_attach(void) { GW_LOG("USB ECM attach"); return HAL_OK; }
static int cdc_ecm_cb_setup(void) { GW_LOG("USB ECM setup"); return HAL_OK; }

static int cdc_ecm_cb_detach(void)
{
	GW_LOG("USB ECM detach");
#if CONFIG_USBH_CDC_ECM_HOTPLUG
	BaseType_t woken = pdFALSE;
	cdc_ecm_detach_pending = 1;
	xSemaphoreGiveFromISR(cdc_ecm_detach_sema, &woken);
	portYIELD_FROM_ISR(woken);
#endif
	return HAL_OK;
}

static int cdc_ecm_cb_bulk_receive(u8 *buf, u32 len)
{
	if (len > 0) {
		usbh_ecm_rx_buf = buf;
		ethernetif_mii_recv(&xnetif[ETHERNET_IDX], (int)len);
	}
	return HAL_OK;
}

static int cdc_ecm_cb_process(usb_host_t *host, u8 msg)
{
	(void)host;
	if (msg == USBH_MSG_USER_SET_CONFIG) {
		usbh_cdc_ecm_choose_config(host);
	}
	return HAL_OK;
}

static int cdc_ecm_cb_device_check(usb_host_t *host, u8 cfg_max)
{
	(void)cfg_max;
	return usbh_cdc_ecm_check_config_desc(host);
}

static const usbh_config_t usbh_ecm_cfg = {
	.speed = USB_SPEED_HIGH,
	.isr_priority = 4U,
	.main_task_stack_size = 1280U,
	.main_task_priority = tskIDLE_PRIORITY + 5,
	.tick_source = USBH_SOF_TICK,
	.class_num = 1U,
	.rx_fifo_depth = 1712,
	.nptx_fifo_depth = 256,
	.ptx_fifo_depth = 256,
};

static const usbh_cdc_ecm_state_cb_t cdc_ecm_usb_cb = {
	.init = cdc_ecm_cb_init,
	.deinit = cdc_ecm_cb_deinit,
	.attach = cdc_ecm_cb_attach,
	.detach = cdc_ecm_cb_detach,
	.setup = cdc_ecm_cb_setup,
	.bulk_received = cdc_ecm_cb_bulk_receive,
};

static const usbh_user_cb_t usbh_ecm_usr_cb = {
	.process = cdc_ecm_cb_process,
	.validate = cdc_ecm_cb_device_check,
};

static const usbh_cdc_ecm_priv_data_t ecm_priv = { NULL, NULL, 0 };

/* lwIP <-> USB 以太网 的收发桥（契约同 SDK 的 ethernetif_mii_init） */
s8 rltk_mii_send(struct eth_drv_sg *sg_list, int sg_len, int total_len)
{
	struct eth_drv_sg *last_sg;
	u8 *pdata = usbh_ecm_tx_buf;
	int ret;

	if ((total_len < 0) || ((u32)total_len > sizeof(usbh_ecm_tx_buf))) {
		return -1;
	}
	for (last_sg = &sg_list[sg_len]; sg_list < last_sg; ++sg_list) {
		memcpy(pdata, (void *)(sg_list->buf), sg_list->len);
		pdata += sg_list->len;
	}
	ret = usbh_cdc_ecm_send_data(usbh_ecm_tx_buf, (u32)total_len, 1);
	return (ret == HAL_OK) ? 0 : -1;
}

void rltk_mii_recv(struct eth_drv_sg *sg_list, int sg_len)
{
	struct eth_drv_sg *last_sg;
	u8 *pbuf = usbh_ecm_rx_buf;

	for (last_sg = &sg_list[sg_len]; sg_list < last_sg; ++sg_list) {
		if (sg_list->buf != 0) {
			memcpy((void *)(sg_list->buf), pbuf, sg_list->len);
			pbuf += sg_list->len;
		}
	}
}

int usb_ethernet_transmit(u8 *buf, u32 len, u8 block)
{
	return usbh_cdc_ecm_send_data(buf, len, block);
}

static int cdc_ecm_do_init(void)
{
	int ret;

	ret = usbh_init(&usbh_ecm_cfg, &usbh_ecm_usr_cb);
	if (ret != HAL_OK) {
		GW_LOG("usbh_init fail");
		return 0;
	}
	ret = usbh_cdc_ecm_init(&cdc_ecm_usb_cb, &ecm_priv);
	if (ret != HAL_OK) {
		GW_LOG("usbh_cdc_ecm_init fail");
		usbh_deinit();
		return 0;
	}
	usbh_start();
	while (!usbh_cdc_ecm_usb_is_ready()) {
		vTaskDelay(pdMS_TO_TICKS(200));
	}
	usbh_cdc_ecm_prepare_done();
	GW_LOG("USB ECM ready");
	return 1;
}

/* ================================================================== LAN 侧配置（相机网段 + DHCP 服务器） */
static void gw_setup_lan(void)
{
	u32_t sta, mask, base;
	unsigned char *ip;
	struct ip_addr pool_start, pool_end;

	ip = LwIP_GetIP(WLAN_IDX);
	if (ip == NULL) {
		return;
	}
	sta = (u32_t)ip[0] << 24 | (u32_t)ip[1] << 16 | (u32_t)ip[2] << 8 | (u32_t)ip[3];
	if (sta == 0) {
		GW_LOG("STA 还没有 IP，稍后再配 LAN");
		return;
	}
	mask = (u32_t)LwIP_GetMASK(WLAN_IDX)[0] << 24 | (u32_t)LwIP_GetMASK(WLAN_IDX)[1] << 16 |
		   (u32_t)LwIP_GetMASK(WLAN_IDX)[2] << 8 | (u32_t)LwIP_GetMASK(WLAN_IDX)[3];
	base = (sta & mask) & 0xFFFFFF00u;

	s_ecm_ip_nbo = htonl(base | GW_ECM_HOST_OCTET);
	s_pool_first = base | GW_POOL_FIRST;
	s_pool_last = base | GW_POOL_LAST;

	GW_LOG("LAN: ecm=%lu.%lu.%lu.%lu  pool=%lu.%lu.%lu.%lu-%lu",
		   (base >> 24) & 0xFF, (base >> 16) & 0xFF, (base >> 8) & 0xFF, (u32_t)GW_ECM_HOST_OCTET,
		   (base >> 24) & 0xFF, (base >> 16) & 0xFF, (base >> 8) & 0xFF, (u32_t)GW_POOL_FIRST,
		   (u32_t)GW_POOL_LAST);

	/* ECM 口设成同网段（这样相机发往网关的包直接到我们；其余靠代理 ARP） */
	LwIP_SetIP(ETHERNET_IDX, s_ecm_ip_nbo, htonl(mask), s_ecm_ip_nbo);
	netif_set_link_up(&xnetif[ETHERNET_IDX]);
	netif_set_up(&xnetif[ETHERNET_IDX]);

	/* DHCP 服务器：把相机地址池限制在 .240-.247，网关/DNS 都指向我们自己 */
	dhcps_init(&xnetif[ETHERNET_IDX]);
	pool_start.addr = htonl(s_pool_first);
	pool_end.addr = htonl(s_pool_last);
	dhcps_set_addr_pool(1, &pool_start, &pool_end);

	/* 默认路由必须留在 WiFi（出网走路由器），千万别切到 ECM 口 */
	netifapi_netif_set_default(&xnetif[WLAN_IDX]);

	s_lan_ready = 1;
	GW_LOG("LAN ready, DHCP server on");
}

/* ================================================================== WiFi STA */
static rtw_result_t gw_scan_result_handler(unsigned int scanned_AP_num, void *user_data);

static void gw_wifi_scan_start(void)
{
	rtw_scan_param_t scan_param;

	if (s_scan_running) {
		return;
	}
	rtw_memset(&scan_param, 0, sizeof(scan_param));
	scan_param.scan_user_callback = gw_scan_result_handler;
	s_scan_done = 0;
	s_scan_running = 1;
	if (wifi_scan_networks(&scan_param, 0) != RTW_SUCCESS) {
		GW_LOG("wifi_scan_networks fail");
		s_scan_running = 0;
		s_scan_done = 1;
	}
}

static rtw_result_t gw_scan_result_handler(unsigned int scanned_AP_num, void *user_data)
{
	char *scan_buf;
	unsigned int i, cnt;

	(void)user_data;
	if (scanned_AP_num == 0 || scanned_AP_num > 200) {
		s_ap_count = 0;
		s_scan_running = 0;
		s_scan_done = 1;
		return RTW_SUCCESS;
	}
	scan_buf = (char *)rtw_zmalloc(scanned_AP_num * sizeof(rtw_scan_result_t));
	if (scan_buf == NULL) {
		s_scan_running = 0;
		s_scan_done = 1;
		return RTW_ERROR;
	}
	cnt = scanned_AP_num;
	if (wifi_get_scan_records(&cnt, scan_buf) >= 0) {
		int k = 0;
		for (i = 0; i < cnt && k < GW_MAX_AP; i++) {
			rtw_scan_result_t *r = (rtw_scan_result_t *)(scan_buf + i * sizeof(rtw_scan_result_t));
			if (r->SSID.len == 0 || r->SSID.len >= GW_SSID_LEN) {
				continue;
			}
			r->SSID.val[r->SSID.len] = 0;
			memset(s_aps[k].ssid, 0, GW_SSID_LEN);
			memcpy(s_aps[k].ssid, (char *)r->SSID.val, r->SSID.len);
			s_aps[k].rssi = r->signal_strength;
			s_aps[k].sec = (r->security != RTW_SECURITY_OPEN);
			k++;
		}
		s_ap_count = k;
		GW_LOG("scan done, %d AP", k);
	}
	rtw_mfree((u8 *)scan_buf, 0);
	s_scan_running = 0;
	s_scan_done = 1;
	return RTW_SUCCESS;
}

static int gw_wifi_connect_try(const char *ssid, const char *pass, int security_type)
{
	rtw_network_info_t p;
	int ret;

	rtw_memset(&p, 0, sizeof(p));
	memcpy(p.ssid.val, ssid, strlen(ssid));
	p.ssid.len = strlen(ssid);
	p.password = (unsigned char *)pass;
	p.password_len = strlen(pass);
	p.security_type = security_type;
	ret = wifi_connect(&p, 1); /* block=1：连上或超时才返回 */
	return (ret == RTW_SUCCESS) ? 0 : -1;
}

static void gw_wifi_connect_now(void)
{
	int i;
	unsigned char *ip;

	s_wifi_state = GW_STATE_CONNECTING;
	GW_LOG("connecting to '%s' ...", s_ssid);

	if (wifi_on(RTW_MODE_STA) < 0) {
		GW_LOG("wifi_on fail");
		s_wifi_state = GW_STATE_FAILED;
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(500));

	if (s_pass[0] == 0) {
		if (gw_wifi_connect_try(s_ssid, "", RTW_SECURITY_OPEN) != 0) {
			s_wifi_state = GW_STATE_FAILED;
			GW_LOG("connect(open) fail");
			return;
		}
	} else {
		/* 依次尝试常见的几种，覆盖 WPA2/WPA 混合、AES/TKIP 等情况 */
		if (gw_wifi_connect_try(s_ssid, s_pass, RTW_SECURITY_WPA2_AES_PSK) != 0 &&
			gw_wifi_connect_try(s_ssid, s_pass, RTW_SECURITY_WPA2_MIXED_PSK) != 0 &&
			gw_wifi_connect_try(s_ssid, s_pass, RTW_SECURITY_WPA_AES_PSK) != 0) {
			s_wifi_state = GW_STATE_FAILED;
			GW_LOG("connect fail (all security types)");
			return;
		}
	}
	GW_LOG("wifi connected, waiting IP ...");
	LwIP_DHCP(WLAN_IDX, DHCP_START);
	for (i = 0; i < 40; i++) {
		vTaskDelay(pdMS_TO_TICKS(250));
		ip = LwIP_GetIP(WLAN_IDX);
		if (ip && ip[0] != 0) {
			break;
		}
	}
	ip = LwIP_GetIP(WLAN_IDX);
	if (ip == NULL || ip[0] == 0) {
		s_wifi_state = GW_STATE_FAILED;
		GW_LOG("no IP from router");
		return;
	}
	snprintf(s_wifi_ip, sizeof(s_wifi_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
	s_wifi_state = GW_STATE_CONNECTED;
	GW_LOG("STA IP = %s", s_wifi_ip);
	gw_setup_lan();
}

static void gw_wifi_task(void *param)
{
	(void)param;
	GW_LOG("wifi task start");
	vTaskDelay(pdMS_TO_TICKS(500));
	if (wifi_on(RTW_MODE_STA) < 0) {
		GW_LOG("wifi_on returned error (可能已经开过了)");
	} else {
		GW_LOG("wifi_on ok");
	}
	for (;;) {
		if (s_want_disconnect) {
			s_want_disconnect = 0;
			wifi_disconnect();
			s_wifi_state = GW_STATE_IDLE;
			s_wifi_ip[0] = 0;
			GW_LOG("disconnected");
		}
		if (s_want_connect) {
			s_want_connect = 0;
			gw_wifi_connect_now();
		}
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}

/* ================================================================== 控制 API（自己写的极简 HTTP 服务） */
static int gw_http_read_request(int sock, char *buf, int buflen)
{
	int total = 0;
	int n;
	char *hdr_end;
	int content_len = 0;

	while (total < buflen - 1) {
		n = lwip_recv(sock, buf + total, buflen - 1 - total, 0);
		if (n <= 0) {
			break;
		}
		total += n;
		buf[total] = 0;
		hdr_end = strstr(buf, "\r\n\r\n");
		if (hdr_end != NULL) {
			char *cl = strstr(buf, "Content-Length:");
			if (cl == NULL) {
				cl = strstr(buf, "content-length:");
			}
			if (cl != NULL) {
				content_len = atoi(cl + 15);
			}
			if (total >= (int)(hdr_end + 4 - buf) + content_len) {
				break;
			}
		}
	}
	return total;
}

static void gw_http_send(int sock, int code, const char *body)
{
	char head[256];
	int body_len = body ? (int)strlen(body) : 0;
	int head_len;

	head_len = snprintf(head, sizeof(head),
						"HTTP/1.1 %d %s\r\n"
						"Content-Type: application/json\r\n"
						"Content-Length: %d\r\n"
						"Connection: close\r\n"
						"Access-Control-Allow-Origin: *\r\n"
						"\r\n",
						code, (code == 200) ? "OK" : "Error", body_len);
	lwip_send(sock, head, head_len, 0);
	if (body_len > 0) {
		lwip_send(sock, (void *)body, body_len, 0);
	}
}

/* 从 {"ssid":"x","pass":"y"} 里取字段（够用即可，支持 \" 和 \\ 转义） */
static int gw_json_get(const char *json, const char *key, char *out, int outlen)
{
	char pat[32];
	const char *p;
	int i = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (p == NULL) {
		return -1;
	}
	p = strchr(p + strlen(pat), ':');
	if (p == NULL) {
		return -1;
	}
	p++;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '"') {
		return -1;
	}
	p++;
	while (*p && *p != '"' && i < outlen - 1) {
		if (*p == '\\' && p[1]) {
			p++;
		}
		out[i++] = *p++;
	}
	out[i] = 0;
	return 0;
}

static void gw_http_handle(int sock)
{
	static char req[1400];
	static char resp[1900];
	char method[8] = {0};
	char path[160] = {0};
	char *body;
	int n;

	n = gw_http_read_request(sock, req, sizeof(req));
	if (n <= 0) {
		return;
	}
	if (sscanf(req, "%7s %159s", method, path) != 2) {
		gw_http_send(sock, 400, "{\"ok\":0}");
		return;
	}
	body = strstr(req, "\r\n\r\n");
	if (body) {
		body += 4;
	} else {
		body = req + n;
	}

	if (strcmp(path, "/api/sys/info") == 0 || strcmp(path, "/") == 0) {
		unsigned char *ip = LwIP_GetIP(ETHERNET_IDX);
		snprintf(resp, sizeof(resp),
				 "{\"ok\":1,\"model\":\"LiThermal-BW21\",\"mode\":\"proxy-arp\","
				 "\"ecm\":\"%d.%d.%d.%d\",\"conn\":\"%s\",\"ip\":\"%s\"}",
				 ip ? ip[0] : 0, ip ? ip[1] : 0, ip ? ip[2] : 0, ip ? ip[3] : 0,
				 s_ssid[0] ? s_ssid : "", s_wifi_ip);
		gw_http_send(sock, 200, resp);

	} else if (strcmp(path, "/api/wifi/scan") == 0) {
		int i, k = 0;
		int off;
		/* 触发一次扫描并等结果（相机那边读超时是 6s，这里最多等 4.5s） */
		gw_wifi_scan_start();
		for (i = 0; i < 45 && !s_scan_done; i++) {
			vTaskDelay(pdMS_TO_TICKS(100));
		}
		off = snprintf(resp, sizeof(resp), "{\"ok\":1,\"aps\":[");
		for (i = 0; i < s_ap_count && i < GW_MAX_AP; i++) {
			int w = snprintf(resp + off, sizeof(resp) - off,
							 "%s{\"ssid\":\"%.32s\",\"rssi\":%d,\"sec\":%d}",
							 (k > 0) ? "," : "", s_aps[i].ssid, s_aps[i].rssi, s_aps[i].sec);
			if (w <= 0 || off + w >= (int)sizeof(resp) - 8) {
				break;
			}
			off += w;
			k++;
		}
		snprintf(resp + off, sizeof(resp) - off, "]}");
		gw_http_send(sock, 200, resp);

	} else if (strcmp(path, "/api/wifi/connect") == 0) {
		char ssid[GW_SSID_LEN] = {0};
		char pass[GW_PASS_LEN] = {0};
		/* 也支持 GET 用 query 传：?ssid=..&pass=.. */
		char *q = strchr(path, '?');
		if (gw_json_get(body, "ssid", ssid, sizeof(ssid)) != 0 && q) {
			char *p = strstr(q, "ssid=");
			if (p) {
				char *amp;
				p += 5;
				amp = strchr(p, '&');
				if (amp) {
					*amp = 0;
				}
				snprintf(ssid, sizeof(ssid), "%s", p);
			}
		}
		gw_json_get(body, "pass", pass, sizeof(pass));
		if (ssid[0] == 0) {
			gw_http_send(sock, 400, "{\"ok\":0,\"msg\":\"no ssid\"}");
		} else {
			snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
			snprintf(s_pass, sizeof(s_pass), "%s", pass);
			s_want_connect = 1;
			s_wifi_state = GW_STATE_CONNECTING;
			GW_LOG("connect request: ssid='%s'", s_ssid);
			gw_http_send(sock, 200, "{\"ok\":1}");
		}

	} else if (strcmp(path, "/api/wifi/disconnect") == 0) {
		s_want_disconnect = 1;
		gw_http_send(sock, 200, "{\"ok\":1}");

	} else if (strcmp(path, "/api/wifi/status") == 0) {
		const char *st = "idle";
		if (s_wifi_state == GW_STATE_CONNECTING) {
			st = "connecting";
		} else if (s_wifi_state == GW_STATE_CONNECTED) {
			st = "connected";
		} else if (s_wifi_state == GW_STATE_FAILED) {
			st = "failed";
		}
		snprintf(resp, sizeof(resp),
				 "{\"ok\":1,\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"lan\":%d,\"aps\":%d}",
				 st, s_ssid, s_wifi_ip, s_lan_ready, s_ap_count);
		gw_http_send(sock, 200, resp);

	} else {
		gw_http_send(sock, 404, "{\"ok\":0,\"msg\":\"not found\"}");
	}
}

static void gw_http_task(void *param)
{
	int srv, c;
	struct sockaddr_in addr;
	struct sockaddr_in caddr;
	socklen_t clen;

	(void)param;
	vTaskDelay(pdMS_TO_TICKS(1500));

	srv = lwip_socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		GW_LOG("http socket fail: %d", srv);
		vTaskDelete(NULL);
		return;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(GW_CTRL_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (lwip_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		GW_LOG("http bind fail");
		lwip_close(srv);
		vTaskDelete(NULL);
		return;
	}
	lwip_listen(srv, 4);
	GW_LOG("control API on %d", GW_CTRL_PORT);

	for (;;) {
		clen = sizeof(caddr);
		c = lwip_accept(srv, (struct sockaddr *)&caddr, &clen);
		if (c < 0) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}
		gw_http_handle(c);
		lwip_close(c);
	}
}

/* ================================================================== 链路状态 / 热插拔 */
static void gw_link_thread(void *param)
{
	u32_t link_down_cnt = 0;
	u8 link_is_up;
	int inited = 0;

	(void)param;
	GW_LOG("link thread start");
	if (cdc_ecm_do_init() == 0) {
		GW_LOG("usb init fail");
		vTaskDelete(NULL);
		return;
	}
	inited = 1;

	for (;;) {
#if CONFIG_USBH_CDC_ECM_HOTPLUG
		if (cdc_ecm_detach_pending) {
			cdc_ecm_detach_pending = 0;
			netif_set_link_down(&xnetif[ETHERNET_IDX]);
			s_lan_ready = 0;
			inited = 0;
			GW_LOG("ECM unplugged");
		}
#endif
		link_is_up = usbh_cdc_ecm_get_link_status();
		if (link_is_up && !inited) {
			u8 *mac = (u8 *)usbh_cdc_ecm_process_mac_str();
			if (mac) {
				memcpy(xnetif[ETHERNET_IDX].hwaddr, mac, 6);
				netif_set_link_up(&xnetif[ETHERNET_IDX]);
				netif_set_up(&xnetif[ETHERNET_IDX]);
				inited = 1;
				GW_LOG("ECM link up, MAC %02x:%02x:%02x:%02x:%02x:%02x",
					   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
				/* 相机插上后再配 LAN（此时 WiFi 大概率已经有 IP） */
				gw_setup_lan();
			}
		} else if (!link_is_up && inited) {
			inited = 0;
			s_lan_ready = 0;
			netif_set_link_down(&xnetif[ETHERNET_IDX]);
			GW_LOG("ECM link down");
		} else {
			if (!link_is_up && (++link_down_cnt % 20 == 1)) {
				GW_LOG("waiting for camera (ECM link down, %lu)", link_down_cnt);
			}
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

#if CONFIG_USBH_CDC_ECM_HOTPLUG
static void gw_hotplug_thread(void *param)
{
	(void)param;
	for (;;) {
		xSemaphoreTake(cdc_ecm_detach_sema, portMAX_DELAY);
		GW_LOG("hotplug: re-init USB host");
		usbh_stop();
		usbh_cdc_ecm_deinit();
		usbh_deinit();
		vTaskDelay(pdMS_TO_TICKS(50));
		if (cdc_ecm_do_init() == 0) {
			GW_LOG("hotplug re-init fail");
			break;
		}
	}
}
#endif

/* ================================================================== 入口 */
void li_thermal_gw_start(void)
{
	TaskHandle_t t;

	GW_LOG("LiThermal BW21 gateway starting");

	/* 注意：不要在这里调 wifi_on()！这里是框架的 app 任务，栈很小，
	   WiFi 初始化（HALMAC 等）会直接把它撑爆 → Bus Fault。
	   WiFi 初始化放到下面 gw_wifi_task（4KB 栈）里做。 */

#if CONFIG_USBH_CDC_ECM_HOTPLUG
	cdc_ecm_detach_sema = xSemaphoreCreateBinary();
	if (xTaskCreate(gw_hotplug_thread, "li_gw_hotplug", ECM_HOTPLUG_THREAD_STACK_SIZE,
					NULL, ECM_HOTPLUG_THREAD_PRIORITY, NULL) != pdPASS) {
		GW_LOG("create hotplug task fail");
	}
#endif
	GW_LOG("create link task");
	if (xTaskCreate(gw_link_thread, "li_gw_link", ECM_LINK_THREAD_STACK_SIZE,
					NULL, ECM_LINK_THREAD_PRIORITY, &t) != pdPASS) {
		GW_LOG("create link task fail");
	}
	GW_LOG("create wifi task");
	if (xTaskCreate(gw_wifi_task, "li_gw_wifi", GW_WIFI_THREAD_STACK,
					NULL, GW_THREAD_PRIORITY, &t) != pdPASS) {
		GW_LOG("create wifi task fail");
	}
	GW_LOG("create http task");
	if (xTaskCreate(gw_http_task, "li_gw_http", GW_HTTP_THREAD_STACK,
					NULL, GW_THREAD_PRIORITY, &t) != pdPASS) {
		GW_LOG("create http task fail");
	}
	GW_LOG("all tasks started");
}
