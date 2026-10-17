// LiOS 桌面 —— 公共声明
//
// 这是一个**独立程序**（不属于 LiThermal）。开机由 thermalcamera.sh 直接启动，
// 它就是这台相机的「桌面 / 家」；相机只是桌面上一个方块：
//
//   lios（桌面）
//     └── fork + exec /mnt/UDISK/LiThermal   ← 在 worker 线程里 waitpid，绝不在 LVGL 线程等
//
// 两条必须守住的纪律（都踩过坑）：
//   1) LVGL 事件回调 / LVGL 定时器回调里**绝不加锁、绝不做文件 I/O**
//      （事件是在 lv_timer_handler 内部派发的，那时锁已经在手里；
//       非递归锁会自锁死整机，写文件实测会卡住）
//   2) UI 线程起来之前创建的 LVGL 定时器不会被触发 —— 所以桌面在 UI 线程里建

#pragma once

#include <lvgl.h>
#include <pthread.h>
#include <stdbool.h>

// ---------------------------------------------------------------- 锁
// 递归锁：即使有人在 LVGL 回调里误写 LOCKLV()，也最多多一次引用计数，不会锁死整机
extern pthread_mutex_t g_lv_mutex;
#define LOCKLV() pthread_mutex_lock(&g_lv_mutex)
#define UNLOCKLV() pthread_mutex_unlock(&g_lv_mutex)

// ---------------------------------------------------------------- 字体
// 和相机用同一套生成结果（各自编进各自的可执行文件，互不依赖）
extern "C"
{
    extern const lv_font_t ui_font_cn16;
    extern const lv_font_t ui_font_cn12;
}

// ---------------------------------------------------------------- 日志（固定写 /tmp，不写 SD 卡）
void lios_log(const char *fmt, ...);

// ---------------------------------------------------------------- 按键事件
// 输入线程只置标志，UI 线程消费（UI 线程才碰 LVGL）
enum
{
    LIOS_K_MENU = 0, // 菜单键（KEY_F2）
    LIOS_K_REC,      // 录像键（KEY_F3）
    LIOS_K_ENTER,    // 波轮按下（KEY_ENTER）
    LIOS_K_POWER,    // 电源键（KEY_F1）
    LIOS_K_CNT
};
bool lios_key_take(int k); // 取走一次「按下」事件
void lios_key_flush(void); // 丢掉所有待处理事件

// ---------------------------------------------------------------- 相机子进程
bool lios_child_running(void);      // 相机是否在前台（前台时桌面不碰显示）
void lios_launch_camera(void);      // fork+exec 相机；worker 线程里 waitpid
bool lios_need_redraw_take(void);   // 相机退出后需要整屏重绘

// ---------------------------------------------------------------- 看门狗
void lios_beat(void); // UI 线程每转一圈调一次

// ---------------------------------------------------------------- 桌面
typedef struct
{
    const char *id;     // 唯一 id（权限记录、插件都会用）
    const char *name;   // 方块上显示的名字
    const char *icon;   // 1~2 个汉字/符号
    lv_obj_t *(*create)(lv_obj_t *parent); // 建页面；NULL = 用「建设中」占位页
    bool wheel_focus;   // true = 页面里让滚轮/波轮按下走 LVGL 焦点（列表类页面要）
} lios_app_def_t;

extern const lios_app_def_t LIOS_APPS[];
extern const int LIOS_APP_COUNT;

// 页面钩子：真页面自己接管按键 / 每帧 / 销毁（都在 UI 线程、已持锁时调用）
// 注意：这些回调里**绝不做文件 I/O、绝不 LOCKLV()**（锁已在外层持有）
typedef struct
{
    // 返回 true = 这次按键页面自己处理了（桌面不要关页面）
    bool (*key)(int key);
    void (*tick)(void);    // 每帧（取 worker 结果、更新界面）
    void (*destroy)(void); // 页面关闭前
} lios_page_hooks_t;

void lios_page_set_hooks(const lios_page_hooks_t *hooks);

// 文件管理器页面（src/lios/files.cpp）
void page_files_create(lv_obj_t *parent);

void lios_desktop_create(void);   // UI 线程首次调用（已持锁）
void lios_desktop_tick(void);     // 每帧调用（已持锁）：返回键 + 电源长按
void lios_desktop_redraw_all(void); // 相机退出后整屏重绘（已持锁）
bool lios_desktop_page_open(void);  // 当前是否在某个应用页里（调试/测试用）

// ---------------------------------------------------------------- 电源
void lios_poweroff(void);        // 长按电源键
bool lios_power_key_check(void); // UI 线程每帧调；长按到点就关机并返回 true
