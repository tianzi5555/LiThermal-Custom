#include "my_main.h"
#include "uiTrace.h"
#include "statusBar.h"
#include "lios_user.h"     // LiOS 第一期：开机密码 + 两种顶层模式（--login）
#include "lios_launcher.h" // LiOS 第二期：真桌面 + 双向模式切换
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
    // LiOS 第二期：切到桌面时相机线程先待机（不读图不刷界面），回相机后自动恢复
    while (cameraUtils.connected == false || lios_mode_active())
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
        // LiOS 第二期：桌面显示期间暂停相机 UI 刷新（回相机后循环自己继续）
        if (lios_mode_active())
        {
            usleep(100000);
            continue;
        }
        /* 注意：getTemperature() 已经不在这里同步调用了（搬到 thread_temp_poll_func）。
           原来每轮同步等一次 HTTP，实测 getTemp_avg≈97ms / max≈262ms，把这条本该 25Hz 的
           UI 更新循环压到 ~7.4Hz —— 这就是「卡顿」的主因。现在主循环只读缓存值，
           UI 更新回到 ~24Hz；温度数值新鲜度由轮询线程保证（不低于原来）。 */
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
    // LiOS 第二期：桌面显示期间待机，回相机后继续（原逻辑一律不改）
    while (cameraUtils.connected == false || lios_mode_active())
        usleep(100000);
    while (1)
    {
        if (lios_mode_active())
        {
            usleep(100000);
            continue;
        }
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

/// @brief 温度轮询线程：把 getTemperature() 的 HTTP 阻塞从主循环里搬出来
///
/// 原来主循环每轮同步等一次 HTTP，实测 getTemp_avg≈97ms / max≈262ms，把本该 25Hz 的
/// UI 更新循环压到 ~7.4Hz —— 用户说的「卡顿」主因。
/// 现在 HTTP 只在这条线程里跑（周期 120ms：比原来的实际周期 ~136ms 还快一点，
/// 所以温度数值新鲜度不降），主循环只读 copyTemperatureResult() 的缓存值，
/// UI 更新率回到 ~24Hz。LVGL 对象的更新仍然全部在主循环持 lv_mutex 的路径里做。
#define TEMP_POLL_MS 120
pthread_t thread_temp_poll;
void *thread_temp_poll_func(void *)
{
    // LiOS 第二期：桌面显示期间待机，回相机后继续（原逻辑一律不改）
    while (cameraUtils.connected == false || lios_mode_active())
        usleep(100000);
    while (1)
    {
        if (lios_mode_active())
        {
            usleep(100000);
            continue;
        }
        uint32_t t0 = perf_probe_now_us();
        if (current_mode == MODE_MAINPAGE || current_mode == MODE_CAMERA_SETTINGS)
        {
            cameraUtils.getTemperature();
        }
        // 和中心温度线程一样：把请求耗时从周期里扣掉，保证是固定周期
        uint32_t used_ms = (perf_probe_now_us() - t0) / 1000;
        if (used_ms < TEMP_POLL_MS)
            usleep((useconds_t)(TEMP_POLL_MS - used_ms) * 1000);
        perf_probe_add(PERF_TEMP_PERIOD, perf_probe_now_us() - t0);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    // 只读自检：`LiThermal --selftest-pw` —— 把 lios.conf 的解析结果和三条密码判定写进
    // /tmp/lios.log（只输出长度/校验和，不建界面、不改任何文件），排查密码分流用。
    for (int i = 1; i < argc; i++)
    {
        if (argv[i] != NULL && strcmp(argv[i], "--selftest-pw") == 0)
            return lios_selftest_pw();
    }

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

    // ---- LiOS 第一期：开机密码 + 两种顶层模式 ----
    // 不带 --login 时 lios_user_prepare 返回 false，下面这段整段跳过，
    // 行为与改动前**完全一致**（绝不影响现有部署）。
    bool enter_lios_mode = lios_debug_lios_requested(argc, argv); // --debug-lios 直接进桌面（调试）
    if (!enter_lios_mode && lios_user_prepare(argc, argv))
    {
        current_mode = MODE_LOGIN; // 让滚轮/按键交给密码界面（见 myHAL.cpp: encoder_read）
        lios_mode_t m = lios_login_show_and_run();
        enter_lios_mode = (m == LIOS_ENTER_LIOS);
        printf("[lios] 进入%s\n", enter_lios_mode ? " LiOS 模式" : "相机");
    }

    // ---- 相机启动（幂等）----
    // 由 enter_camera() 统一负责：waitboot 连接动画、各 UI 部件、相机线程。
    // 第二期起相机/C 桌面可以双向往返，所以这里不再直接建线程。
    if (enter_lios_mode)
    {
        // LiOS 模式：先让 launcher 线程去建桌面；桌面退出后它自己会调 enter_camera()
        current_mode = MODE_LIOS;
        // 自动测试默认关闭，只有 /tmp/lios_autotest 存在才跑；
        // 看门狗在 enter_camera() 起完 UI 线程后自动启动（那时心跳才有意义）
        lios_autotest_start();
        lios_enter();
        printf("[lios] 已切到 LiOS 桌面（退出桌面后自动启动相机）\n");
    }
    else
    {
        current_mode = MODE_MAINPAGE;
        enter_camera();
    }

    void *result;
    // LiOS 启动路径下 UI 线程是 launcher 线程建的，等它建出来再 join。
    // （直接 pthread_join(thread_ui) 有 join 到 0 的风险，那是未定义行为、实测会崩）
    while (!lios_camera_ready())
        usleep(20000);
    if (thread_ui != 0)
        pthread_join(thread_ui, &result);
    return 0;
}
