#ifndef UI_STATUSBAR_H
#define UI_STATUSBAR_H

// 主界面顶部半透明状态条：CPU 占用 / 内存占用
// （CPU 温度本机取不到：内核没编 THS 驱动，/sys/class/thermal 不存在）
#ifdef __cplusplus
extern "C" {
#endif

void ui_statusbar_create(void);

#ifdef __cplusplus
}
#endif

#endif
