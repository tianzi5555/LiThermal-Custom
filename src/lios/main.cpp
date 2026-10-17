// LiOS 桌面 —— 程序入口
//
// 开机由 /mnt/UDISK/thermalcamera.sh 直接启动本程序（它就是这台相机的桌面）。
// 这里做四件事：
//   1) 极简 LVGL + 显示初始化（复用 libs/lv_drivers 的 sunxifb，不碰相机那套 myHAL）
//   2) 输入线程：只读 /dev/input，把按键变成标志位（UI 线程才碰 LVGL）
//   3) UI 线程：lv_task_handler + 桌面每帧逻辑（唯一碰 LVGL 的线程）
//   4) 看门狗：UI 心跳停 3 秒 → 写日志 + _exit(99)，让启动脚本重新拉起来

#include "lios.h"
#include "lios_perm.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lv_drivers/display/sunxifb.h"

// 电源管理（复用相机那份串口实现，只用到 init / powerOff）
void PowerManager_init();
void PowerManager_powerOff();

// 字体（和相机同一套生成结果）
extern "C" const lv_font_t ui_font_cn16;
extern "C" const lv_font_t ui_font_cn12;

#define LIOS_LOG_PATH "/tmp/lios.log"
#define LIOS_CAMERA_BIN "/mnt/UDISK/LiThermal"
#define LIOS_INPUT_ENCODER "/dev/input/event0"
#define LIOS_INPUT_KEY "/dev/input/event2"

pthread_mutex_t g_lv_mutex;

// ---------------------------------------------------------------- 日志

void lios_log(const char *fmt, ...)
{
    FILE *f = fopen(LIOS_LOG_PATH, "a");
    if (f == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    fflush(NULL);
}

// ---------------------------------------------------------------- 按键标志

static volatile bool s_key[LIOS_K_CNT] = {false, false, false, false};
static volatile bool s_enc_pressed = false;
static volatile int s_enc_diff = 0;

bool lios_key_take(int k)
{
    if (k < 0 || k >= LIOS_K_CNT)
        return false;
    if (!s_key[k])
        return false;
    s_key[k] = false;
    return true;
}

void lios_key_flush(void)
{
    for (int i = 0; i < LIOS_K_CNT; i++)
        s_key[i] = false;
    s_enc_diff = 0;
}

// 滚轮差值：正 = 下/右。只有 UI 线程会取（LVGL 的 indev read_cb 或桌面 tick），
// 二者互斥：indev 开着时由 read_cb 取走，关着时由桌面 tick 取走。
int lios_enc_take(void)
{
    int d = s_enc_diff;
    s_enc_diff = 0;
    return d;
}

bool lios_enc_pressed(void)
{
    return s_enc_pressed;
}

// ---------------------------------------------------------------- 相机子进程

static volatile bool s_child_running = false;
static volatile bool s_need_redraw = false;

bool lios_child_running(void)
{
    return s_child_running;
}

bool lios_need_redraw_take(void)
{
    if (!s_need_redraw)
        return false;
    s_need_redraw = false;
    return true;
}

static void *lios_camera_thread(void *)
{
    lios_log("相机：fork + exec %s", LIOS_CAMERA_BIN);
    pid_t pid = fork();
    if (pid == 0)
    {
        // 子进程：只跑相机
        execl(LIOS_CAMERA_BIN, LIOS_CAMERA_BIN, (char *)NULL);
        _exit(127); // exec 失败
    }
    if (pid < 0)
    {
        lios_log("相机：fork 失败（%s），回桌面", strerror(errno));
        s_child_running = false;
        s_need_redraw = true;
        return NULL;
    }

    int status = 0;
    for (;;)
    {
        pid_t r = waitpid(pid, &status, 0);
        if (r == pid || (r < 0 && errno != EINTR))
            break;
    }
    lios_log("相机：已退出（status=0x%x），回桌面并整屏重绘", status);
    lios_key_flush();
    s_child_running = false;
    s_need_redraw = true;
    return NULL;
}

void lios_launch_camera(void)
{
    if (s_child_running)
    {
        lios_log("相机：已经在运行，忽略本次点击");
        return;
    }
    lios_key_flush();
    s_child_running = true; // 立刻停掉桌面渲染，把显示让给相机

    pthread_t th;
    if (pthread_create(&th, NULL, lios_camera_thread, NULL) != 0)
    {
        lios_log("相机：创建 worker 线程失败");
        s_child_running = false;
        return;
    }
    pthread_detach(th);
}

// ---------------------------------------------------------------- 看门狗

static volatile uint32_t s_beat = 0;
static volatile bool s_wd_stop = false;

void lios_beat(void)
{
    s_beat++;
}

// ---------------------------------------------------------------- 电源

static volatile uint32_t s_power_press_ms = 0; // 电源键按下时刻（0 = 没按）

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

void lios_poweroff(void)
{
    lios_log("长按电源键 -> 关机");
    FILE *f = fopen("/tmp/poweroff", "w");
    if (f != NULL)
    {
        fputs("1", f);
        fclose(f);
    }
    sync();
    PowerManager_powerOff(); // 内部会 poweroff + exit(0)
    _exit(0);                // 兜底
}

// 返回 true 表示这次电源键已被关机流程吃掉
bool lios_power_key_check(void)
{
    if (s_power_press_ms == 0)
        return false;
    if (now_ms() - s_power_press_ms < 1500)
        return false;
    s_power_press_ms = 0;
    lios_poweroff();
    return true;
}

// ---------------------------------------------------------------- 输入线程

static void *lios_thread_encoder(void *)
{
    int fd = open(LIOS_INPUT_ENCODER, O_RDONLY);
    if (fd < 0)
    {
        lios_log("输入：打不开 %s（滚轮不可用）", LIOS_INPUT_ENCODER);
        return NULL;
    }
    struct input_event ev;
    while (1)
    {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev))
        {
            usleep(10000);
            continue;
        }
        if (ev.type != EV_REL)
            continue;
        if (s_child_running)
            continue; // 相机在前台：事件交给相机，桌面不累加
        // 方向与相机一致（相机里是 encoder_new_move -= value）
        s_enc_diff -= ev.value;
    }
    return NULL;
}

static void *lios_thread_keys(void *)
{
    int fd = open(LIOS_INPUT_KEY, O_RDONLY);
    if (fd < 0)
    {
        lios_log("输入：打不开 %s（按键不可用）", LIOS_INPUT_KEY);
        return NULL;
    }
    struct input_event ev;
    while (1)
    {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev))
        {
            usleep(10000);
            continue;
        }
        if (ev.type != EV_KEY)
            continue;

        if (ev.code == KEY_ENTER)
        {
            s_enc_pressed = (ev.value == 1);
            if (ev.value == 1 && !s_child_running)
                s_key[LIOS_K_ENTER] = true;
            continue;
        }
        if (ev.code < KEY_F1 || ev.code > KEY_F3)
            continue;

        int idx = ev.code - KEY_F1; // 0=电源 1=菜单 2=录像
        if (idx == 0)
        {
            // 电源键：记按下时刻，长按判定在 UI 线程里做（关机要碰文件/串口，别在输入线程里跑）
            if (ev.value == 1)
                s_power_press_ms = now_ms();
            else
            {
                uint32_t held = (s_power_press_ms != 0) ? (now_ms() - s_power_press_ms) : 0;
                s_power_press_ms = 0;
                if (held > 0 && held < 800)
                    s_power_short_ms = now_ms(); // 电源短按（组合键用）
            }
            continue;
        }
        if (ev.value != 1)
            continue;
        if (s_child_running)
            continue; // 相机在前台：按键归相机
        if (idx == 1)
            s_key[LIOS_K_MENU] = true;
        else if (idx == 2)
            s_key[LIOS_K_REC] = true;
    }
    return NULL;
}

// ---------------------------------------------------------------- 显示 / 输入设备初始化

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_color_t *s_fb_buf = NULL;
static uint32_t s_width = 0, s_height = 0;

// 截图/录屏用的帧访问（capture.cpp 调用）
lv_color_t *lios_fb_buf(void) { return s_fb_buf; }
uint32_t lios_fb_w(void) { return s_width; }
uint32_t lios_fb_h(void) { return s_height; }
void lios_lock_display(void) { LOCKLV(); }
void lios_unlock_display(void) { UNLOCKLV(); }

static int lios_display_init(void)
{
    lv_init();

    uint32_t rotated = LV_DISP_ROT_NONE;
    sunxifb_init(rotated);
    sunxifb_get_sizes(&s_width, &s_height);
    lios_log("显示：%ux%u", (unsigned)s_width, (unsigned)s_height);
    if (s_width == 0 || s_height == 0)
        return -1;

    s_fb_buf = (lv_color_t *)sunxifb_alloc(s_width * s_height * sizeof(lv_color_t),
                                          const_cast<char *>("lios"));
    if (s_fb_buf == NULL)
    {
        lios_log("显示：sunxifb_alloc 失败");
        return -1;
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_fb_buf, NULL, s_width * s_height);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = s_width;
    s_disp_drv.ver_res = s_height;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = sunxifb_flush;
    s_disp_drv.rotated = rotated;
    lv_disp_drv_register(&s_disp_drv);
    return 0;
}

// 编码器输入：这个回调是在 lv_timer_handler 内部被调用的（那时锁已在手里），
// 所以这里**只读几个 volatile 变量**，绝不加锁、绝不做 I/O
static void lios_encoder_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->enc_diff = (int16_t)lios_enc_take();
    data->state = lios_enc_pressed() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void lios_indev_init(void)
{
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_ENCODER;
    indev_drv.read_cb = lios_encoder_read;
    lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    lv_indev_set_group(indev, group);
}

// 截图/录屏（capture.cpp 实现）：电源+录像=截图；电源+菜单=录屏
void lios_capture_screenshot(void);
void lios_capture_rec_toggle(void);
static volatile uint32_t s_power_short_ms = 0; // 电源短按释放时刻（0=无）

// ---------------------------------------------------------------- UI 线程

static void *lios_thread_ui(void *)
{
    bool first = true;
    while (1)
    {
        // 相机在前台：桌面完全不碰显示（相机自己在画），只保持心跳
        if (s_child_running)
        {
            lios_beat();
            usleep(30000);
            continue;
        }

        LOCKLV();
        if (first)
        {
            lios_desktop_create();
            first = false;
        }
        if (s_need_redraw)
        {
            s_need_redraw = false;
            lios_desktop_redraw_all();
        }
        lios_beat();
        lv_task_handler();
        lios_desktop_tick();
        // 组合键：电源短按 + 录像 = 截图；电源短按 + 菜单 = 录屏开/停
        if (s_power_short_ms != 0 && (now_ms() - s_power_short_ms) < 600)
        {
            if (lios_key_take(LIOS_K_REC))
            {
                s_power_short_ms = 0;
                lios_capture_screenshot();
            }
            else if (lios_key_take(LIOS_K_MENU))
            {
                s_power_short_ms = 0;
                lios_capture_rec_toggle();
            }
            else if ((now_ms() - s_power_short_ms) > 500)
                s_power_short_ms = 0;
        }
        UNLOCKLV();
        usleep(5000);
    }
    return NULL;
}

// ---------------------------------------------------------------- 看门狗线程

static void *lios_thread_watchdog(void *)
{
    uint32_t last = 0;
    int stuck = 0;
    usleep(1500 * 1000); // 给 UI 线程一点启动时间
    last = s_beat;
    while (!s_wd_stop)
    {
        usleep(500 * 1000);
        if (s_wd_stop)
            break;
        if (s_child_running)
        {
            // 相机在前台时桌面是「暂停」的，不算卡死
            last = s_beat;
            stuck = 0;
            continue;
        }
        if (s_beat == last)
        {
            if (++stuck >= 6) // 3 秒
            {
                lios_log("!!! 看门狗：UI 心跳停了 3 秒（疑似死锁），强制退出");
                sync();
                _exit(99);
            }
        }
        else
        {
            stuck = 0;
            last = s_beat;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- main

int main(void)
{
    // 递归锁
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lv_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    lios_log("=== lios 启动（桌面）pid=%d ===", (int)getpid());

    if (lios_display_init() != 0)
    {
        lios_log("显示初始化失败，退出（启动脚本会兜底跑相机）");
        return 1;
    }
    lios_indev_init();

    PowerManager_init(); // 只为长按电源键关机（复用相机那份串口实现）

    // 扫描 /mnt/UDISK/apps/*.so 与 /mnt/UDISK/*.so（后台线程做，UI 线程绝不做 I/O）
    lios_apps_scan_start();
    lios_perm_init(); // 启动就加载权限记录（拦截要用）

    pthread_t t;
    if (pthread_create(&t, NULL, lios_thread_encoder, NULL) == 0)
        pthread_detach(t);
    if (pthread_create(&t, NULL, lios_thread_keys, NULL) == 0)
        pthread_detach(t);
    if (pthread_create(&t, NULL, lios_thread_watchdog, NULL) == 0)
        pthread_detach(t);

    if (pthread_create(&t, NULL, lios_thread_ui, NULL) != 0)
    {
        lios_log("UI 线程创建失败，退出");
        return 1;
    }
    pthread_join(t, NULL);
    return 0;
}
