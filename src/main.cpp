#include "my_main.h"
#include "uiTrace.h"
#include "wifiManager.h"
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
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
    while (1)
    {
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

/// @brief 中心点温度刷新线程：用原始 readJpegWithExtra 读取变焦中心点温度，1 秒一次
void *thread_center_temp_func(void *)
{
    while (cameraUtils.connected == false)
        usleep(100000);
    while (1)
    {
        if (current_mode == MODE_MAINPAGE &&
            g_last_crop[2] > 0 && g_last_crop[3] > 0 &&
            g_last_crop[4] > 0 && g_last_crop[5] > 0)
        {
            int cx = (g_last_crop[0] + g_last_crop[2] / 2) * 160 / g_last_crop[4];
            int cy = (g_last_crop[1] + g_last_crop[3] / 2) * 120 / g_last_crop[5];
            cameraUtils.readJpegWithExtra(NULL, cx, cy);
        }
        usleep(1000000);
    }
    return NULL;
}

int main()
{
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
    pthread_create(&thread_ui, NULL, thread_ui_func, NULL);
    cameraUtils.initHTTPClient();
    // 开机就把网络后端线程拉起来：探测 BW21 + 用已保存凭据自动连接。
    // 这样不用先去点 WiFi 页面，插上 BW21 就能自动上网。
    wifi_init(NULL);
    pthread_create(&thread_app, NULL, thread_app_func, NULL);
    pthread_create(&thread_center_temp, NULL, thread_center_temp_func, NULL);
    void *result;
    pthread_join(thread_ui, &result);
    return 0;
}
