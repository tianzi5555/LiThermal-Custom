// 可复用的屏幕软键盘（3 页：小写 / 大写 / 符号）+ 输入框
// 从原来的 WiFi 页面里抽出来的，终端（SH 终端）也用它。

#include "uiKeyboard.h"
#include "my_main.h" // ui_font_cn16

#include <string.h>
#include <stdio.h>

#define KB_ROWS 4
#define KB_COLS 10
#define KB_N (KB_ROWS * KB_COLS)
#define KB_KEY_W 26
#define KB_KEY_H 24
#define KB_PITCH_X 29
#define KB_PITCH_Y 26
#define KB_INPUT_H 26

static const char *PAGE0[KB_N] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
    "k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
    "u", "v", "w", "x", "y", "z", "1", "2", "3", "4",
    "5", "6", "7", "8", "9", "0", ".", "_", "-", "@"};
static const char *PAGE1[KB_N] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "!", "#", "$", "%",
    "&", "*", "(", ")", "+", "=", ";", ":", "'", "\""};
static const char *PAGE2[KB_N] = {
    "~", "|", "\\", "/", "?", ",", "<", ">", "[", "]",
    "{", "}", "`", "^", "%", "&", "*", "(", ")", "+",
    "=", ";", ":", "'", "\"", "!", "#", "$", "_", "-",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};

struct ui_kb
{
    lv_obj_t *root;
    lv_obj_t *ta;
    lv_obj_t *lbl_len;
    lv_obj_t *btn_page;
    lv_obj_t *keys[KB_N];
    int page;
    ui_kb_action_cb cb;
    void *user;
};

static const char *const *page_map(int p)
{
    if (p == 1)
        return PAGE1;
    if (p == 2)
        return PAGE2;
    return PAGE0;
}
static const char *page_name(int p)
{
    if (p == 1)
        return "大写";
    if (p == 2)
        return "符号";
    return "小写";
}

static void set_btn_text(lv_obj_t *btn, const char *t)
{
    if (!btn || !lv_obj_is_valid(btn))
        return;
    lv_obj_t *lab = lv_obj_get_child(btn, 0);
    if (lab && lv_obj_is_valid(lab))
        lv_label_set_text(lab, t ? t : "");
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3D4C58), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lab);
    return btn;
}

static void update_len(ui_kb_t *kb)
{
    if (!kb->lbl_len || !lv_obj_is_valid(kb->lbl_len))
        return;
    const char *t = lv_textarea_get_text(kb->ta);
    lv_label_set_text_fmt(kb->lbl_len, "%d", (int)(t ? strlen(t) : 0));
}

static void kb_key_cb(lv_event_t *e)
{
    ui_kb_t *kb = (ui_kb_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (!kb || idx < 0 || idx >= KB_N)
        return;
    const char *ch = page_map(kb->page)[idx];
    if (ch && ch[0])
        lv_textarea_add_text(kb->ta, ch);
    update_len(kb);
}

static void kb_page_cb(lv_event_t *e)
{
    ui_kb_t *kb = (ui_kb_t *)lv_event_get_user_data(e);
    if (!kb)
        return;
    kb->page = (kb->page + 1) % 3;
    const char *const *map = page_map(kb->page);
    for (int i = 0; i < KB_N; i++)
        set_btn_text(kb->keys[i], map[i]);
    set_btn_text(kb->btn_page, page_name(kb->page));
}

static void kb_space_cb(lv_event_t *e)
{
    ui_kb_t *kb = (ui_kb_t *)lv_event_get_user_data(e);
    if (kb)
    {
        lv_textarea_add_text(kb->ta, " ");
        update_len(kb);
    }
}
static void kb_del_cb(lv_event_t *e)
{
    ui_kb_t *kb = (ui_kb_t *)lv_event_get_user_data(e);
    if (kb)
    {
        lv_textarea_del_char(kb->ta);
        update_len(kb);
    }
}
static void kb_clear_cb(lv_event_t *e)
{
    ui_kb_t *kb = (ui_kb_t *)lv_event_get_user_data(e);
    if (kb)
    {
        lv_textarea_set_text(kb->ta, "");
        update_len(kb);
    }
}
static void kb_action_cb(lv_event_t *e)
{
    ui_kb_t *kb = (ui_kb_t *)lv_event_get_user_data(e);
    if (!kb)
        return;
    if (kb->cb)
        kb->cb(lv_textarea_get_text(kb->ta), kb->user);
}

ui_kb_t *ui_kb_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t w,
                      const char *action_label, ui_kb_action_cb cb, void *user)
{
    ui_kb_t *kb = (ui_kb_t *)lv_mem_alloc(sizeof(ui_kb_t));
    if (!kb)
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

    // 输入框 + 字符计数
    kb->ta = lv_textarea_create(kb->root);
    lv_obj_set_size(kb->ta, w - 34, KB_INPUT_H - 4);
    lv_obj_set_pos(kb->ta, 0, 0);
    lv_textarea_set_one_line(kb->ta, true);
    lv_textarea_set_max_length(kb->ta, 512);
    lv_obj_set_style_text_font(kb->ta, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(kb->ta, lv_color_hex(0x11181F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(kb->ta, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(kb->ta, LV_OBJ_FLAG_SCROLLABLE);

    kb->lbl_len = lv_label_create(kb->root);
    lv_obj_set_pos(kb->lbl_len, w - 30, 6);
    lv_obj_set_style_text_font(kb->lbl_len, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(kb->lbl_len, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(kb->lbl_len, "0");

    // 40 个字符键
    kb->page = 0;
    const char *const *map = page_map(0);
    lv_coord_t y0 = KB_INPUT_H;
    for (int r = 0; r < KB_ROWS; r++)
    {
        for (int c = 0; c < KB_COLS; c++)
        {
            int i = r * KB_COLS + c;
            lv_obj_t *b = make_btn(kb->root, map[i], c * KB_PITCH_X, y0 + r * KB_PITCH_Y, KB_KEY_W, KB_KEY_H);
            lv_obj_set_user_data(b, (void *)(intptr_t)i);
            lv_obj_add_event_cb(b, kb_key_cb, LV_EVENT_CLICKED, kb);
            kb->keys[i] = b;
        }
    }

    // 功能行
    lv_coord_t fy = y0 + KB_ROWS * KB_PITCH_Y;
    lv_coord_t bw = 47, gap = 1;
    kb->btn_page = make_btn(kb->root, page_name(0), 0, fy, bw, 24);
    lv_obj_add_event_cb(kb->btn_page, kb_page_cb, LV_EVENT_CLICKED, kb);
    lv_obj_t *b;
    b = make_btn(kb->root, "空格", (bw + gap) * 1, fy, bw, 24);
    lv_obj_add_event_cb(b, kb_space_cb, LV_EVENT_CLICKED, kb);
    b = make_btn(kb->root, "删", (bw + gap) * 2, fy, bw, 24);
    lv_obj_add_event_cb(b, kb_del_cb, LV_EVENT_CLICKED, kb);
    b = make_btn(kb->root, "清空", (bw + gap) * 3, fy, bw, 24);
    lv_obj_add_event_cb(b, kb_clear_cb, LV_EVENT_CLICKED, kb);
    b = make_btn(kb->root, action_label ? action_label : "确定", (bw + gap) * 4, fy, bw, 24);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E7F4F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(b, kb_action_cb, LV_EVENT_CLICKED, kb);

    return kb;
}

const char *ui_kb_text(ui_kb_t *kb)
{
    return (kb && kb->ta) ? lv_textarea_get_text(kb->ta) : "";
}
void ui_kb_set_text(ui_kb_t *kb, const char *s)
{
    if (!kb || !kb->ta)
        return;
    lv_textarea_set_text(kb->ta, s ? s : "");
    update_len(kb);
}
void ui_kb_clear(ui_kb_t *kb)
{
    ui_kb_set_text(kb, "");
}
lv_obj_t *ui_kb_textarea(ui_kb_t *kb)
{
    return kb ? kb->ta : NULL;
}
void ui_kb_focus(ui_kb_t *kb)
{
    if (kb && kb->keys[0] && lv_obj_is_valid(kb->keys[0]))
        lv_group_focus_obj(kb->keys[0]);
}
