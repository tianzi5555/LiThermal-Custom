#include "my_main.h"
#include "uiTrace.h"
#include "statusBar.h"
#include "perfProbe.h" // 临时性能探针（诊断用，可删）
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
pthread_mutex_t lv_mutex;

// 崩溃记录：Tina 的 uClibc 没有 execinfo.h/backtrace，所以只记信号 + 出错地址，
// 配合 uiTrace（/mnt/UDISK/ui.log 分步日志）就能定位崩在哪一步。
static void crash_signal_handler(int sig, siginfo_t *si, void *uctx)
{
    (void)uctx;
    int fd = open("/mnt/UDISK/crash.log", O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd >= 0)
    {
        char buf[192];
        int len = snprintf(buf, sizeof(buf),
                           "\n=== LiThermal crash: signal %d, fault addr %p ===\n",
                           sig, (si != NULL) ? si->si_addr : NULL);
        if (len > 0)
            write(fd, buf, (size_t)len);
        fsync(fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/// @brief 热成像刷新线程
pthread_t thread_app;
void *thread_app_func(void *)
{
    static uint32_t last_color_palette = -1;
    while (cameraUtils.connected == false)
        usleep(100000);
    sleep(1);
    // 关闭 4117 自带叠加，避免数码变焦把 OSD 放大/裁花
    cameraUtils.setCenterMeasure(false);
    cameraUtils.set4117Cursor(false, false);
    LOCKLV();
    widget_graph_updateSettings();
    ui_crosshairs_updateVisibility();
    ui_center_display_updateVisibility();
    widget_graph_check_visibility();
    ui_zoom_pip_check_visibility();
    UNLOCKLV();
    uint32_t perf_prev_loop_start = 0;
    while (1)
    {
        /* --- perf probe: 主循环每轮耗时（不含尾部 usleep） + 轮与轮的真实间隔 --- */
        uint32_t perf_loop_t0 = perf_probe_now_us();
        if (perf_prev_loop_start != 0)
            perf_probe_add(PERF_LOOP_PERIOD, perf_loop_t0 - perf_prev_loop_start);
        perf_prev_loop_start = perf_loop_t0;

        if (last_color_palette != globalSettings.colorPalette)
        {
            last_color_palette = globalSettings.colorPalette;
            cameraUtils.setColorPalette(globalSettings.colorPalette);
        }
        if (current_mode == MODE_MAINPAGE || current_mode == MODE_CAMERA_SETTINGS)
        {
            cameraUtils.getTemperature();
        }
        LOCKLV();
        ui_crosshairs_updatePos();
        ui_center_display_update();
        ui_center_display_updateVisibility();
        widget_graph_check_visibility();
        ui_zoom_pip_check_visibility();
        UNLOCKLV();
        perf_probe_add(PERF_LOOP, perf_probe_now_us() - perf_loop_t0);
        usleep(40000);
    }
    return NULL;
}
pthread_t thread_ui;
pthread_t thread_center_temp;
void *thread_ui_func(void *)
{
    HAL::lv_loop();
}

/// @brief 中心点温度刷新线程
///
/// 中心温度只能从模组的 jpegPicWithAppendData 取（一次响应约 100KB：JPEG + 160x120 原始测温矩阵），
/// 它跟 25Hz 的 getTemperature() 原本共用一把锁，两边互相排队，所以看着「刷新很慢」。
/// 现在 readJpegWithExtra 走独立连接/独立锁（见 cameraUtils.cpp），这条链路只做两件事：
///   1) 只在真正需要这个读数的时候才去打扰模组（主界面 + 中心温度显示开着）；
///   2) 按固定周期跑，间隔可调。
/// 间隔越小刷新越快、但给模组的压力越大。1000 = 原来的速度；改成 2000 更省模组。
#define CENTER_TEMP_FETCH_MS 500

void *thread_center_temp_func(void *)
{
    while (cameraUtils.connected == false)
        usleep(100000);
    while (1)
    {
        /* --- perf probe: 中心温度线程每轮周期（含固定 sleep，用来确认周期真的对） --- */
        uint32_t perf_center_t0 = perf_probe_now_us();

        struct timeval begin_tv;
        gettimeofday(&begin_tv, NULL);

        // 中心温度标签被关掉（或不在主界面）时标签本来就是隐藏的，读数没人看，
        // 这时完全不必每秒去让模组生成一张 100KB 的大响应。
        if (current_mode == MODE_MAINPAGE &&
            globalSettings.enableCenterValueDisplay &&
            g_last_crop[2] > 0 && g_last_crop[3] > 0 &&
            g_last_crop[4] > 0 && g_last_crop[5] > 0)
        {
            int cx = (g_last_crop[0] + g_last_crop[2] / 2) * 160 / g_last_crop[4];
            int cy = (g_last_crop[1] + g_last_crop[3] / 2) * 120 / g_last_crop[5];
            cameraUtils.readJpegWithExtra(NULL, cx, cy);
        }

        // 把本次请求耗掉的时间从周期里扣掉，保证是「固定周期」而不是「请求 + 整周期 sleep」
        struct timeval end_tv;
        gettimeofday(&end_tv, NULL);
        long used_ms = (end_tv.tv_sec - begin_tv.tv_sec) * 1000L + (end_tv.tv_usec - begin_tv.tv_usec) / 1000L;
        if (used_ms < 0)
            used_ms = 0;
        if (used_ms < CENTER_TEMP_FETCH_MS)
            usleep((useconds_t)(CENTER_TEMP_FETCH_MS - used_ms) * 1000);
        perf_probe_add(PERF_CENTER, perf_probe_now_us() - perf_center_t0);
    }
    return NULL;
}

int main()
{
    perf_probe_init(); // 临时性能探针：/tmp/perf.log，每 1 秒汇总一行（诊断用，可删）
    install_crash_handler();
    ui_trace_reset();
    sleep(1); // Why?
    system("mkdir " GALLERY_PATH);
    system("mkdir " GALLERY2_PATH);
    pthread_mutex_init(&lv_mutex, NULL);
    HAL::init();
    readFiles(GALLERY_PATH);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lv_layer_sys(), LV_OBJ_FLAG_SCROLLABLE);
    printf("Loop begin\n");
    waitboot_scr_load(lv_scr_act());
    widget_graph_create();
    ui_crosshairs_create();
    ui_center_display_create();
    ui_statusbar_create(); // 顶部半透明状态条（CPU/内存）
    pthread_create(&thread_ui, NULL, thread_ui_func, NULL);
    cameraUtils.initHTTPClient();
    pthread_create(&thread_app, NULL, thread_app_func, NULL);
    pthread_create(&thread_center_temp, NULL, thread_center_temp_func, NULL);
    void *result;
    pthread_join(thread_ui, &result);
    return 0;
}
