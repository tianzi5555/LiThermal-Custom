/*
 * li_video_stream.c —— BW21 摄像头 → LiThermal 相机 的 USB 视频链路固件
 * Copyright (c) 2026 LiThermal (GPL-3.0)
 *
 * 拓扑（点对点，不需要 DHCP / 代理 ARP / IP 转发，因此【不打任何 SDK 补丁】）：
 *
 *   [LiThermal 相机] USB gadget = CDC-ECM（相机侧自带 f_ecm）→ IP 192.168.2.231/24
 *   [BW21]          USB Host = usbh_cdc_ecm → IP 192.168.2.250/24
 *   画面：板载摄像头(1080p30 sensor) → ISP → JPEG 编码(VIDEO_JPEG) → MMF2
 *         → 本例程的 VS_TCP sink 模块 → TCP :5555 → 相机
 *   帧格式：[4 字节大端长度][JPEG 数据]（连续字节流）
 *
 * 分辨率说明：相机屏幕只有 320×240，所以默认流 VGA(640×480)@30fps——
 *   相机侧解码负载小、延迟低。要更清晰可把 VS_RESOLUTION 改成 VIDEO_HD/VIDEO_FHD。
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

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* MMF2 视频 */
#include "mmf2_module.h"
#include "mmf2_siso.h"
#include "module_video.h"
#include "mmf2_pro2_video_config.h"
#include "sensor.h"
#include "video_api.h"

#include "example_li_video_stream.h"

#define VS_LOG(fmt, ...) printf("[LiVS] " fmt "\n", ##__VA_ARGS__)

/* ------------------------------------------------------------------ 参数 */
extern struct netif xnetif[NET_IF_NUM];
#define ETHERNET_IDX (NET_IF_NUM - 1)

#define VS_IP_NBO   0xC0A802FAu   /* 192.168.2.250（BW21 侧） */
#define VS_MASK_NBO 0xFFFFFF00u   /* 255.255.255.0           */
#define VS_PORT     5555

/* 视频：默认 VGA@30（相机屏 320×240 足够）；想要更清晰改 VIDEO_HD / VIDEO_FHD */
#define VS_RESOLUTION VIDEO_VGA
#define VS_WIDTH  640
#define VS_HEIGHT 480
#define VS_FPS    30
#define VS_BPS    (2 * 1024 * 1024)

/* ------------------------------------------------------------------ 通用状态 */
static volatile int s_net_ready = 0;

/* ================================================================== 1) USB Host + CDC-ECM
 * 整段从 li_thermal_gw / 官方 usbh_cdc_ecm_new 例程移植：回调 + lwIP 桥（契约同
 * ethernetif_mii_init：netif->linkoutput → rltk_mii_send；bulk_receive → ethernetif_mii_recv）
 */

#define CONFIG_USBH_CDC_ECM_HOTPLUG 1

static u8 *usbh_ecm_rx_buf;
static u8 usbh_ecm_tx_buf[1536] __attribute__((aligned(CACHE_LINE_SIZE)));

#if CONFIG_USBH_CDC_ECM_HOTPLUG
static u8 cdc_ecm_detach_pending;
static SemaphoreHandle_t cdc_ecm_detach_sema;
#endif

static int cdc_ecm_cb_init(void) { VS_LOG("USB ECM init"); return HAL_OK; }
static int cdc_ecm_cb_deinit(void) { VS_LOG("USB ECM deinit"); return HAL_OK; }
static int cdc_ecm_cb_attach(void) { VS_LOG("USB ECM attach"); return HAL_OK; }
static int cdc_ecm_cb_setup(void) { VS_LOG("USB ECM setup"); return HAL_OK; }

static int cdc_ecm_cb_detach(void)
{
	VS_LOG("USB ECM detach");
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

/* lwIP <-> USB 以太网 桥（契约同 SDK 的 ethernetif_mii_init） */
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
		VS_LOG("usbh_init fail");
		return 0;
	}
	ret = usbh_cdc_ecm_init(&cdc_ecm_usb_cb, &ecm_priv);
	if (ret != HAL_OK) {
		VS_LOG("usbh_cdc_ecm_init fail");
		usbh_deinit();
		return 0;
	}
	usbh_start();
	while (!usbh_cdc_ecm_usb_is_ready()) {
		vTaskDelay(pdMS_TO_TICKS(200));
	}
	usbh_cdc_ecm_prepare_done();
	VS_LOG("USB ECM ready");
	return 1;
}

/* ================================================================== 2) TCP 服务端
 * 相机是客户端。一次只服务一个连接；新连接会踢掉旧的。
 * 每帧：[4 字节大端长度][JPEG 数据]
 */
static int s_listen_fd = -1;
static int s_client_fd = -1;
static SemaphoreHandle_t s_fd_mtx = NULL;

static int cli_take(void)
{
	int fd = -1;
	if (s_fd_mtx != NULL && xSemaphoreTake(s_fd_mtx, portMAX_DELAY) == pdTRUE) {
		fd = s_client_fd;
		xSemaphoreGive(s_fd_mtx);
	}
	return fd;
}

static void cli_drop_locked(int fd)
{
	if (s_client_fd == fd && fd >= 0) {
		close(fd);
		s_client_fd = -1;
	}
}

static void tcp_server_task(void *arg)
{
	(void)arg;
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) {
		VS_LOG("socket fail");
		vTaskDelete(NULL);
		return;
	}
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(VS_PORT);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 1) < 0) {
		VS_LOG("bind/listen :%d fail", VS_PORT);
		close(lfd);
		vTaskDelete(NULL);
		return;
	}
	s_listen_fd = lfd;
	VS_LOG("TCP 服务端就绪 :%d，等相机连接…", VS_PORT);

	for (;;) {
		int cfd = accept(lfd, NULL, NULL);
		if (cfd < 0)
			continue;
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000; /* 发送超时 200ms：卡住了就断，让相机重连 */
		setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		if (s_fd_mtx != NULL)
			xSemaphoreTake(s_fd_mtx, portMAX_DELAY);
		if (s_client_fd >= 0 && s_client_fd != cfd) {
			VS_LOG("新连接，踢旧连接 fd=%d", s_client_fd);
			close(s_client_fd);
		}
		s_client_fd = cfd;
		if (s_fd_mtx != NULL)
			xSemaphoreGive(s_fd_mtx);
		VS_LOG("相机已连接 fd=%d", cfd);
	}
}

/* ================================================================== 3) MMF2 输出模块：把帧发出去 */
typedef struct {
	int dummy;
} vs_ctx_t;

static void *vs_create(void *parent)
{
	(void)parent;
	vs_ctx_t *ctx = (vs_ctx_t *)malloc(sizeof(vs_ctx_t));
	return ctx;
}

static void *vs_destroy(void *p)
{
	if (p)
		free(p);
	return NULL;
}

static int vs_control(void *p, int cmd, int arg)
{
	(void)p;
	(void)cmd;
	(void)arg;
	return 0;
}

/* JPEG 帧前可能有 4 字节 meta（VIDEO_JPEG_META_OFFSET）：找 SOI(FFD8) 再发 */
static int vs_handle(void *p, void *input, void *output)
{
	(void)p;
	(void)output;
	mm_queue_item_t *item = (mm_queue_item_t *)input;
	if (item == NULL)
		return 0;
	uint8_t *data = (uint8_t *)item->data_addr;
	uint32_t size = item->size;
	if (data == NULL || size < 64)
		return 0;

	uint32_t off = 0;
	if (!(data[0] == 0xFF && data[1] == 0xD8)) {
		for (uint32_t i = 1; i < 16 && i + 1 < size; i++) {
			if (data[i] == 0xFF && data[i + 1] == 0xD8) {
				off = i;
				break;
			}
		}
	}
	data += off;
	size -= off;
	if (size < 64)
		return 0;

	int fd = cli_take();
	if (fd < 0)
		return 0; /* 没有客户端：丢帧（正常，等相机连接） */

	uint8_t hdr[4];
	hdr[0] = (size >> 24) & 0xFF;
	hdr[1] = (size >> 16) & 0xFF;
	hdr[2] = (size >> 8) & 0xFF;
	hdr[3] = size & 0xFF;

	int err = 0;
	if (send(fd, hdr, 4, 0) != 4)
		err = 1;
	uint32_t sent = 0;
	while (!err && sent < size) {
		int n = send(fd, data + sent, size - sent, 0);
		if (n <= 0) {
			err = 1;
			break;
		}
		sent += (uint32_t)n;
	}

	if (err) {
		VS_LOG("发送失败，断开 fd=%d（相机会重连）", fd);
		if (s_fd_mtx != NULL)
			xSemaphoreTake(s_fd_mtx, portMAX_DELAY);
		cli_drop_locked(fd);
		if (s_fd_mtx != NULL)
			xSemaphoreGive(s_fd_mtx);
	}
	return 0;
}

static mm_module_t vs_tcp_module = {
	.create = vs_create,
	.destroy = vs_destroy,
	.control = vs_control,
	.handle = vs_handle,

	.new_item = NULL,
	.del_item = NULL,

	.output_type = MM_TYPE_NONE,  /* sink：没有输出 */
	.module_type = MM_TYPE_VSINK,
	.name = "VS_TCP"
};

/* ================================================================== 4) 视频链路 */
static video_params_t video_v1_params = {
	.stream_id = 0,
	.type = VIDEO_JPEG,
	.resolution = VS_RESOLUTION,
	.width = VS_WIDTH,
	.height = VS_HEIGHT,
	.bps = VS_BPS,
	.fps = VS_FPS,
	.gop = VS_FPS,
	.rc_mode = 2, /* VBR */
	.direct_output = 1,
	.use_static_addr = 1,
};

static mm_context_t *video_v1_ctx = NULL;
static mm_context_t *vs_ctx = NULL;
static mm_siso_t *siso_video_vs = NULL;

static void video_task(void *param)
{
	(void)param;

	VS_LOG("sensor: %s native %dx%d@%dfps",
		   sensor_params[USE_SENSOR].sensor_name,
		   sensor_params[USE_SENSOR].sensor_width,
		   sensor_params[USE_SENSOR].sensor_height,
		   sensor_params[USE_SENSOR].sensor_fps);

	int voe_heap_size = video_voe_presetting(1, video_v1_params.width, video_v1_params.height,
	                                             VS_BPS, 0,
	                                             0, 0, 0, 0, 0,
	                                             0, 0, 0, 0, 0,
	                                             0, 0, 0);
	VS_LOG("voe heap = %d", voe_heap_size);

	video_v1_ctx = mm_module_open(&video_module);
	if (video_v1_ctx == NULL) {
		VS_LOG("video 模块打开失败");
		vTaskDelete(NULL);
		return;
	}
	mm_module_ctrl(video_v1_ctx, CMD_VIDEO_SET_PARAMS, (int)&video_v1_params);
	mm_module_ctrl(video_v1_ctx, MM_CMD_SET_QUEUE_LEN, video_v1_params.fps * 3);
	mm_module_ctrl(video_v1_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_DYNAMIC);

	vs_ctx = mm_module_open(&vs_tcp_module);
	if (vs_ctx == NULL) {
		VS_LOG("vs_tcp 模块打开失败");
		vTaskDelete(NULL);
		return;
	}

	siso_video_vs = siso_create();
	if (siso_video_vs == NULL) {
		VS_LOG("siso 建链失败");
		vTaskDelete(NULL);
		return;
	}
	siso_ctrl(siso_video_vs, MMIC_CMD_ADD_INPUT, (uint32_t)video_v1_ctx, 0);
	siso_ctrl(siso_video_vs, MMIC_CMD_ADD_OUTPUT, (uint32_t)vs_ctx, 0);
	siso_start(siso_video_vs);

	mm_module_ctrl(video_v1_ctx, CMD_VIDEO_APPLY, 0);
	VS_LOG("视频链路已启动：JPEG %dx%d@%dfps → TCP :%d",
		   video_v1_params.width, video_v1_params.height, video_v1_params.fps, VS_PORT);

	vTaskDelete(NULL);
}

/* ================================================================== 5) 主流程 */
static void main_task(void *param)
{
	(void)param;

	/* lwIP：开机流程若没初始化过（本项目正常启动会跑 LwIP_Init），这里补一次。
	 * CONFIG_ETHERNET=1 时 LwIP_Init 会注册 xnetif[ETHERNET_IDX]。 */
	if (xnetif[ETHERNET_IDX].name[0] == '\0') {
		VS_LOG("lwIP 未初始化，补 LwIP_Init()");
		LwIP_Init();
	}

	if (!cdc_ecm_do_init()) {
		VS_LOG("USB ECM 起不来（检查相机侧 ECM gadget / 线 / VBUS）");
		vTaskDelete(NULL);
		return;
	}

	/* 静态 IP：BW21 = 192.168.2.250，相机 = 192.168.2.231 */
	LwIP_SetIP(ETHERNET_IDX, VS_IP_NBO, VS_MASK_NBO, VS_IP_NBO);
	netif_set_link_up(&xnetif[ETHERNET_IDX]);
	netif_set_up(&xnetif[ETHERNET_IDX]);
	netif_set_default(&xnetif[ETHERNET_IDX]);
	s_net_ready = 1;
	VS_LOG("网络就绪：192.168.2.250/24（相机应为 192.168.2.231）");

	s_fd_mtx = xSemaphoreCreateMutex();

	/* TCP 服务端 */
	if (xTaskCreate(tcp_server_task, "vs_tcp", 1024, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
		VS_LOG("tcp 任务创建失败");
	}

	/* 视频链路（没客户端时丢帧，等相机连接） */
	if (xTaskCreate(video_task, "vs_video", 2048, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
		VS_LOG("video 任务创建失败");
	}

	vTaskDelete(NULL);
}

void li_video_stream_start(void)
{
	VS_LOG("=== li_video_stream 启动（BW21 摄像头 → USB → LiThermal）===");
	if (xTaskCreate(main_task, "vs_main", 2048, NULL, tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
		VS_LOG("主任务创建失败");
	}
}
