#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

// 可复用的屏幕软键盘：4 行 × 10 键，共 3 页（小写 / 大写 / 符号），
// 底部功能行：[翻页] [空格] [删] [清空] [确认键(标签可自定义)]
// 输出：一个输入框（lv_textarea），确认键回调里能拿到当前文本。

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_kb ui_kb_t;
typedef void (*ui_kb_action_cb)(const char *text, void *user);

// parent: 放到哪个容器里；y: 键盘顶部相对 parent 的纵坐标；w: 可用宽度
// action_label: 确认键上的字（例如 "执行" / "连接"）
ui_kb_t *ui_kb_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t w,
                      const char *action_label, ui_kb_action_cb cb, void *user);

const char *ui_kb_text(ui_kb_t *kb);   // 当前输入内容
void ui_kb_set_text(ui_kb_t *kb, const char *s);
void ui_kb_clear(ui_kb_t *kb);
lv_obj_t *ui_kb_textarea(ui_kb_t *kb);
void ui_kb_focus(ui_kb_t *kb);         // 把焦点交给键盘按键（供编码器滚轮使用）

#ifdef __cplusplus
}
#endif

#endif // UI_KEYBOARD_H
