// LiOS 桌面 —— 应用清单
//
// 每个方块对应一项；create 为 NULL 时桌面用「建设中」占位页。
// 做真页面时只要把 create 换成 page_xxx_create 即可，
// 桌面网格 / 返回键 / 相机 fork-exec 都不用动。
//
// wheel_focus：页面里要不要让滚轮/波轮按下走 LVGL 焦点。
//   列表类页面（文件管理器）需要；占位页不需要（免得滚轮去动背后的桌面方块）。
//
// 注意：create 里只能建 LVGL 对象，**不能做文件 I/O**（会在 UI 线程/回调里卡住）。
// 需要读目录之类的事情一律交给 worker 线程（见 files.cpp）。

#include "lios.h"

const lios_app_def_t LIOS_APPS[] = {
    // 相机：不是页面，桌面会 fork+exec /mnt/UDISK/LiThermal
    {"camera", "相机", "相", NULL, false},
    // 文件管理器：真页面（列表类，需要滚轮走焦点）
    {"files", "文件管理器", "文", page_files_create, true},
    // 下面三个还是占位页（第 3/4 批做成真页面）
    {"settings", "设置", "设", NULL, false},
    {"perm", "权限管理", "权", NULL, false},
    {"tasks", "后台", "后", NULL, false},
};

const int LIOS_APP_COUNT = (int)(sizeof(LIOS_APPS) / sizeof(LIOS_APPS[0]));
