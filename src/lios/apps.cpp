// LiOS 桌面 —— 应用清单
//
// 这一批（第 3 轮第 1 批）先只做方块网格 + 相机真启动 + 其余 4 个「建设中」占位页。
// 后面几批做真页面时，只要把对应条目的 create 从 NULL 换成 page_xxx_create 即可，
// 桌面网格 / 返回键 / 相机 fork-exec 都不用动。
//
// 注意：create 里只能建 LVGL 对象，**不能做文件 I/O**（会在 UI 线程/回调里卡住）。
// 需要读目录之类的事情，后面用 worker 线程 + 消息队列做。

#include "lios.h"

const lios_app_def_t LIOS_APPS[] = {
    // 相机：不是页面，桌面会 fork+exec /mnt/UDISK/LiThermal
    {"camera", "相机", "相", NULL},
    // 下面四个这一批是占位页（create == NULL 时桌面用「建设中」页）
    {"files", "文件管理器", "文", NULL},
    {"settings", "设置", "设", NULL},
    {"perm", "权限管理", "权", NULL},
    {"tasks", "后台", "后", NULL},
};

const int LIOS_APP_COUNT = (int)(sizeof(LIOS_APPS) / sizeof(LIOS_APPS[0]));
