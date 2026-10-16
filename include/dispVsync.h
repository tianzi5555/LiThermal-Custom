/**
 * @file dispVsync.h
 *
 * 用 /dev/disp 的 vsync 事件等「帧头」（临时修复用，可整体删除）。
 *
 * 背景（本机 T113 / Tina Linux 5.4.61，源码 dev_disp.c / dev_fb.c）：
 *  - fb0 只有 1 页（yres_virtual=240，disp_reserve=307200），没有第二页可以 pan，
 *    所以 sunxifb.c 的双缓冲路径是死代码，LVGL 只能直写正在扫描输出的 framebuffer。
 *  - 内核 fb 的 FBIO_WAITFORVSYNC 是空实现（dev_fb.c:1182 只剩一句注释），
 *    实测返回 0 且只花 ~20µs —— 不能用它等 vsync。
 *  - 面板 117Hz（8.5ms/帧），整屏 320x240x4=300KB 的 CPU 拷贝实测 ~0.4ms，
 *    比光栅快 ~20 倍：只要在「帧头」开始写，写指针就一路跑在光栅前面，看不见撕裂。
 *  - 可用的 vsync 来源：DISP_VSYNC_EVENT_EN(0x0B) 打开后，内核 disp 的 vsync 线程
 *    每帧用 kobject_uevent 广播 "VSYNC0=<ns>"（dev_disp.c:2239-2242）。
 *    本机没有 mdev/udev、也没有 /proc/sys/kernel/hotplug 助手，所以这条 uevent 通道
 *    不会拉起任何进程，只是把消息广播到 netlink（group 1）。
 *
 * 所以：netlink 收 VSYNC0 消息 = 拿到帧头。为了不会把整台机器挂住，本模块全部是
 * 「尽力而为 + 自动降级」：poll 带超时、连续异常/超时就永久停用并写日志。
 */

#ifndef DISP_VSYNC_H
#define DISP_VSYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/* 打开 /dev/disp、建 netlink 监听、打开 vsync 事件。返回 0 表示可用。幂等。 */
int disp_vsync_init(void);

/* 等下一次 vsync（一定是「新鲜」边沿）。
   返回：等待耗时(µs)；-1 = 超时/不可用（调用方直接跳过，不做任何等待）。 */
int disp_vsync_wait_us(int timeout_ms);

/* 是否仍然可用（自动降级后变 0） */
int disp_vsync_is_available(void);

/* 把 netlink 里积压的 uevent 丢掉（1Hz 调用一次，防止内核 netlink 缓冲被 VSYNC 消息塞满） */
void disp_vsync_drain(void);

void disp_vsync_disable(const char *why);

#ifdef __cplusplus
}
#endif

#endif /* DISP_VSYNC_H */
