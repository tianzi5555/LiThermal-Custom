// LiOS 屏幕键盘实现（一页：a-z / 0-9 / . _ - /）
//
// 4 行 × 10 键 + 底部 [删][清空][确定]；输入框在顶部。
// 所有回调只碰 LVGL + 内存（没有文件 I/O、没有加锁）。

#include "ui_kb.h"
#include "lios.h"

#include <stdio.h>
#include <string.h>

#define KB_COLS 10
#define KB_ROWS 4
#define KB_N (KB_ROWS * KB_COLS)
#define KB_KEY_W 28
#define KB_KEY_H 22
#define KB_PITCH_X 30
#define KB_PITCH_Y 24
#define KB_INPUT_H 28

// a-z / 0-9 / . _ - /  = 26 + 10 + 5 = 41 个字符，取前 40 个（最后一格放 "/"）
static const char *const KB_CHARS[KB_N] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
    "k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
    "u", "v", "w", "x", "y", "z", "0", "1", "2", "3",
    "4", "5", "6", "7", "8", "9", ".", "_", "-", "/"};

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *ta;
    lv_obj_t *keys[KB_N];
    lios_kb_action_cb cb;
    void *user;
} kb_t;

// 输入框不参与焦点（滚轮只在按键之间走，避免误改输入内容）；
// kb->root 的 user_data 存 kb_t*
static kb_t *kb_of(lv_obj_t *kb)
{
    return (kb != NULL) ? (kb_t *)lv_obj_get_user_data(kb) : NULL;
}

static void set_btn_text(lv_obj_t *btn, const char *t)
{
    if (btn == NULL || !lv_obj_is_valid(btn))
        return;
    lv_obj_t *lab = lv_obj_get_child(btn, 0);
    if (lab != NULL && lv_obj_is_valid(lab))
        lv_label_set_text(lab, t ? t : "");
}

static lv_obj_t *make_key(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h, bool accent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_radius(btn, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(accent ? 0x1E7F4F : 0x2B3640),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lab);
    return btn;
}

static void kb_char_cb(lv_event_t *e)
{
    kb_t *kb = (kb_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (kb == NULL || kb->ta == NULL || idx < 0 || idx >= KB_N)
        return;
    const char *ch = KB_CHARS[idx];
    if (ch != NULL && ch[0] != '\0')
        lv_textarea_add_text(kb->ta, ch);
}

static void kb_del_cb(lv_event_t *e)
{
    kb_t *kb = (kb_t *)lv_event_get_user_data(e);
    if (kb != NULL && kb->ta != NULL)
        lv_textarea_del_char(kb->ta);
}

static void kb_clear_cb(lv_event_t *e)
{
    kb_t *kb = (kb_t *)lv_event_get_user_data(e);
    if (kb != NULL && kb->ta != NULL)
        lv_textarea_set_text(kb->ta, "");
}

static void kb_action_cb(lv_event_t *e)
{
    kb_t *kb = (kb_t *)lv_event_get_user_data(e);
    if (kb == NULL || kb->cb == NULL)
        return;
    kb->cb(lv_textarea_get_text(kb->ta), kb->user);
}

lv_obj_t *lios_kb_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t w,
                         const char *action_label, lios_kb_action_cb cb, void *user)
{
    kb_t *kb = (kb_t *)lv_mem_alloc(sizeof(kb_t));
    if (kb == NULL)
        return NULL;
    memset(kb, 0, sizeof(*kb));
    kb->cb = cb;
    kb->user = user;

    kb->root = lv_obj_create(parent);
    lv_obj_set_size(kb->root, w, KB_INPUT_H + KB_ROWS * KB_PITCH_Y + KB_PITCH_Y);
    lv_obj_set_pos(kb->root, 0, y);
    lv_obj_clear_flag(kb->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(kb->root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(kb->root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(kb->root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_user_data(kb->root, kb);

    kb->ta = lv_textarea_create(kb->root);
    lv_obj_set_size(kb->ta, w - 4, KB_INPUT_H);
    lv_obj_set_pos(kb->ta, 0, 0);
    lv_textarea_set_one_line(kb->ta, true);
    lv_textarea_set_max_length(kb->ta, 120);
    lv_obj_clear_flag(kb->ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(kb->ta, LV_OBJ_FLAG_CLICK_FOCUSABLE); // 不参与滚轮焦点
    lv_obj_set_style_text_font(kb->ta, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(kb->ta, lv_color_hex(0x11181F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(kb->ta, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(kb->ta, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(kb->ta, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);

    for (int i = 0; i < KB_N; i++)
    {
        int r = i / KB_COLS, c = i % KB_COLS;
        lv_obj_t *b = make_key(kb->root, KB_CHARS[i], c * KB_PITCH_X,
                               KB_INPUT_H + r * KB_PITCH_Y, KB_KEY_W, KB_KEY_H, false);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, kb_char_cb, LV_EVENT_CLICKED, kb);
        kb->keys[i] = b;
    }

    // 底部功能行：删 / 清空 / 确认
    lv_coord_t fy = KB_INPUT_H + KB_ROWS * KB_PITCH_Y;
    lv_obj_t *b;
    b = make_key(kb->root, "删", 0, fy, 92, KB_KEY_H, false);
    lv_obj_add_event_cb(b, kb_del_cb, LV_EVENT_CLICKED, kb);
    b = make_key(kb->root, "清空", 94, fy, 92, KB_KEY_H, false);
    lv_obj_add_event_cb(b, kb_clear_cb, LV_EVENT_CLICKED, kb);
    b = make_key(kb->root, action_label ? action_label : "确定", 188, fy, 92, KB_KEY_H, true);
    lv_obj_add_event_cb(b, kb_action_cb, LV_EVENT_CLICKED, kb);

    return kb->root;
}

const char *lios_kb_text(lv_obj_t *kb_root)
{
    kb_t *kb = kb_of(kb_root);
    return (kb != NULL && kb->ta != NULL) ? lv_textarea_get_text(kb->ta) : "";
}

void lios_kb_set_text(lv_obj_t *kb_root, const char *s)
{
    kb_t *kb = kb_of(kb_root);
    if (kb != NULL && kb->ta != NULL)
        lv_textarea_set_text(kb->ta, s ? s : "");
}

void lios_kb_clear(lv_obj_t *kb_root)
{
    lios_kb_set_text(kb_root, "");
}

void lios_kb_focus(lv_obj_t *kb_root)
{
    kb_t *kb = kb_of(kb_root);
    if (kb != NULL && kb->keys[0] != NULL && lv_obj_is_valid(kb->keys[0]))
        lv_group_focus_obj(kb->keys[0]);
}
