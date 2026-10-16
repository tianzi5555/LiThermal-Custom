#ifndef LIOS_APP_H
#define LIOS_APP_H

// ============================================================================
//  LiOS 应用接口（插件 ABI）
//
//  目的：以后加新应用 / 新游戏，只要把编译好的 .so 放进 /mnt/UDISK/apps/ 就行，
//        不用重新编译、不用重烧固件。
//
//  一个应用 = 一个 .so，导出下面三个 C 函数 + 一个信息结构体：
//
//      extern "C" const lios_app_info_t lios_app_info;
//      extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent);
//      extern "C" void      lios_app_destroy(void);
//      extern "C" void      lios_app_key(int key, int event);
//
//  桌面（Launcher）会：
//      1) 扫描 /mnt/UDISK/apps/*.so
//      2) dlopen 后读 lios_app_info 拿到名字/图标/排序
//      3) 被选中时创建一张全屏容器，调用 lios_app_create(container)
//      4) 把滚轮/按键转发给 lios_app_key()
//      5) 退出时调用 lios_app_destroy() 再 dlclose（释放内存）
//
//  注意（本机是 musl + 117MB 内存）：
//      * 不要用大块静态缓冲，尽量用 lv_mem / malloc 后及时 free
//      * 退出时必须把所有 LVGL 对象删掉（容器会被销毁，但你自己 malloc 的要自己 free）
// ============================================================================

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 按键 / 滚轮事件 ----
enum
{
    LIOS_KEY_NONE = 0,
    LIOS_KEY_ENTER,   // 确认键
    LIOS_KEY_BACK,    // 菜单键（返回）
    LIOS_KEY_REC,     // 录像键
    LIOS_KEY_POWER,   // 电源键（短按）
    LIOS_KEY_UP,      // 滚轮上
    LIOS_KEY_DOWN,    // 滚轮下
    LIOS_KEY_LEFT,    // 滚轮左（如有）
    LIOS_KEY_RIGHT,   // 滚轮右（如有）
};

enum
{
    LIOS_EV_PRESS = 0, // 按下
    LIOS_EV_LONG,      // 长按
    LIOS_EV_RELEASE,   // 抬起
};

typedef struct
{
    const char *id;      // 唯一 id，例如 "ball"（用于权限存储）
    const char *name;    // 桌面上显示的名字，例如 "弹球游戏"
    const char *icon;    // 图标：目前用 1-2 个字符/符号（例如 "球" / "方" / "文" / "设"）
    int order;           // 桌面排序，小的在前
    int need_fullscreen; // 1 = 需要独占整屏（游戏常用）
} lios_app_info_t;

// 创建界面：parent 是全屏容器，直接在里面建控件即可；返回你最外层对象（可为 NULL）
lv_obj_t *lios_app_create(lv_obj_t *parent);

// 退出前清理：删除自己 malloc 的内存（LVGL 对象由容器统一删）
void lios_app_destroy(void);

// 按键事件：key 取 LIOS_KEY_*，event 取 LIOS_EV_*
void lios_app_key(int key, int event);

#ifdef __cplusplus
}
#endif

#endif // LIOS_APP_H
