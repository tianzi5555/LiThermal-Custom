#pragma once
#include "my_main.h"
namespace HAL
{
    extern bool key_pressed[3];
    extern time_t key_pressed_start_time[3]; // 注意不包括Enter
    extern bool key_press_event[4];          //  包括Enter
    void init();
    void lv_loop();
} // namespace HAL

typedef enum {
    MODE_MAINPAGE,
    MODE_MAINMENU,
    MODE_GALLERY,
    MODE_GALLERY_MENU,
    MODE_CAMERA_SETTINGS,
    MODE_SYSTEM_SETTINGS,
    MODE_TERMINAL,
    // --- LiOS 第一期：两个顶层模式 ---
    MODE_LOGIN, // 开机密码界面（此时相机还没初始化；滚轮要能走焦点）
    MODE_LIOS,  // LiOS 桌面（占位）；滚轮走方块，菜单键回相机
}current_mode_t;
extern current_mode_t current_mode;         // 当前设备所处模式
extern int last_encoder_direction;          // 记录编码器变化，用于亮度调节或相册
