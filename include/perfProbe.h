/**
 * @file perfProbe.h
 *
 * 临时性能计时探针（诊断用，可整体删除：本文件 + src/perfProbe.cpp + 各处的
 * perf_probe_xxx 与 PERF_SCOPE 调用点）。
 *
 * 目的：把「卡顿 / 撕裂」量化，而不是猜。
 *  - 只写 /tmp/perf.log（内存盘 tmpfs），绝不写 /mnt/UDISK，避免日志本身把 SD 卡拖慢。
 *  - 每 1 秒只汇总输出一次（平均/最大/次数），日志对性能的影响可以忽略。
 *  - 计时用 clock_gettime(CLOCK_MONOTONIC)，不做任何阻塞操作。
 */

#ifndef PERF_PROBE_H
#define PERF_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 计时槽位 ---------------- */
/* 用法：t0 = perf_probe_now_us(); ...; perf_probe_add(slot, perf_probe_now_us() - t0); */
enum perf_slot_e
{
    PERF_LOOP = 0,    /* thread_app_func 每轮「干活」耗时（不含尾部 usleep(40000)） */
    PERF_LOOP_PERIOD, /* thread_app_func 轮与轮之间的真实间隔（含 usleep 40ms，正常≈40000+loop） */
    PERF_GETTEMP,     /* cameraUtils.getTemperature() 全程：等 cli_mutex + HTTP + 解析 */
    PERF_SNAP,        /* readJpegWithExtra() 全程：100KB 大快照请求 */
    PERF_GETFRAME,    /* codec_getFrame()：RTSP 收包 + 解码出一帧 */
    PERF_PROC,        /* process_frame_to_buffers()：裁剪 + 最近邻缩放（CPU 逐像素） */
    PERF_LV,          /* HAL::lv_loop 里 lv_task_handler() 全程（含抢 lv_mutex） */
    PERF_FLUSH,       /* sunxifb_flush() 全程（含像素拷贝 / G2D blit / pan） */
    PERF_FLUSH_PAN,   /* sunxifb_flush() 里 cache sync + G2D blit + FBIOPAN_DISPLAY 段 */
    PERF_HAL,         /* thread_hal_func 每轮「干活」耗时 */
    PERF_CENTER,      /* 中心温度线程每轮（含固定 500ms 周期 sleep，用来确认周期真的对） */
    PERF_SLOT_MAX
};

/* ---------------- 计数器（次数） ---------------- */
enum perf_counter_e
{
    PERF_C_FLUSH_LAST = 0, /* lv_disp_flush_is_last() 为真的次数 = 真正「上屏」的帧数 */
    PERF_C_PAN,            /* FBIOPAN_DISPLAY 调用次数（=0 就说明双缓冲根本没生效） */
    PERF_C_G2D_BLIT,       /* sunxifb_g2d_blit_to_fb 调用次数 */
    PERF_C_MAX
};

/* ---------------- 一次性数值（最后一次的值原样输出） ---------------- */
enum perf_gauge_e
{
    PERF_G_VSYNC_RET = 0,   /* FBIO_WAITFORVSYNC 的返回值 */
    PERF_G_VSYNC_ERRNO,     /* 失败时的 errno */
    PERF_G_YOFFSET_READBACK,/* 回读 FBIOGET_VSCREENINFO 的 yoffset（看 pan 到底有没有被驱动接受） */
    PERF_G_MAX
};

/* 打开 /tmp/perf.log 并启动「每 1 秒汇总一次」的线程（幂等，越早调用越好） */
void perf_probe_init(void);

/* 写一行一次性说明（启动时的 framebuffer 几何 / 走的是哪条显示路径等） */
void perf_probe_note(const char *fmt, ...);

/* 单调时钟，单位微秒（无阻塞） */
uint32_t perf_probe_now_us(void);

/* 记一次耗时（微秒） */
void perf_probe_add(int slot, uint32_t us);

/* 计数器 +delta */
void perf_probe_count(int counter, unsigned int delta);

/* 设置一次性数值 */
void perf_probe_gauge(int gauge, unsigned int value);

/* 注册「每秒调用一次」的钩子（汇总线程调用它，用来探测 vsync 等） */
void perf_probe_set_1s_hook(void (*fn)(void));

#ifdef __cplusplus
} /* extern "C" */

/* C++ 便捷 RAII：函数第一行写 PERF_SCOPE(PERF_XXX)，所有 return 路径都会自动记账 */
namespace perfprobe
{
    struct Scope
    {
        int slot;
        uint32_t t0;
        explicit Scope(int s) : slot(s), t0(perf_probe_now_us()) {}
        ~Scope() { perf_probe_add(slot, perf_probe_now_us() - t0); }
    };
}
#define PERF_SCOPE(slot) ::perfprobe::Scope _perf_scope_##slot(slot)
#else
#define PERF_SCOPE(slot) /* C 文件里用手工 perf_probe_now_us()/perf_probe_add() */
#endif

#endif /* PERF_PROBE_H */
