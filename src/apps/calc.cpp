// LiOS 计算器（插件）
// 输入：滚轮左右 = 移动光标；波轮按下 = 按当前键；录像键 = 退格
// 4 列 × 5 行键盘 + 顶部显示区

#include "lios_app.h"
#include "my_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYS_N 19

static const char *kKeys[KEYS_N] = {
    "C", "±", "÷", "×",      // r0
    "7", "8", "9", "−",       // r1
    "4", "5", "6", "+",       // r2
    "1", "2", "3", "⌫",       // r3
    "0", ".", "="              // r4
};
static const int kCols[KEYS_N] = {0,1,2,3, 0,1,2,3, 0,1,2,3, 0,1,2,3, 0,1,2};
static const int kRows[KEYS_N] = {0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4};

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_disp = NULL;
static lv_obj_t *g_keys[KEYS_N] = {0};
static lv_timer_t *g_timer = NULL;

static int g_cur = 0; // 光标索引
static int g_left = 0, g_right = 0, g_enter = 0, g_rec = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static bool g_enter_long = false;

// 计算器状态
static char g_buf[64];
static double g_acc = 0.0;
static char g_op = 0;      // 0 或 + - * /
static bool g_entering = false;

static void key_press(int idx);
static void refresh_disp(void);

static void key_press(int idx)
{
    const char *k = kKeys[idx];
    switch (k[0])
    {
    case 'C':
        g_acc = 0; g_op = 0; g_entering = false;
        g_buf[0] = '0'; g_buf[1] = 0;
        break;
    case '\xc2': // ± (UTF-8 C2 B1)
        if (g_buf[0] == '0' && g_buf[1] == 0) break;
        if (g_buf[0] == '-')
            memmove(g_buf, g_buf + 1, strlen(g_buf));
        else
        {
            memmove(g_buf + 1, g_buf, strlen(g_buf) + 1);
            g_buf[0] = '-';
        }
        break;
    case '\xcf': // ⌫ (UTF-8 CF AB)
        if (g_entering)
        {
            size_t n = strlen(g_buf);
            if (n > 1)
                g_buf[n - 1] = 0;
            else
            {
                g_buf[0] = '0'; g_buf[1] = 0;
                g_entering = false;
            }
        }
        break;
    case '=':
    {
        double cur = atof(g_buf);
        if (g_op && g_entering)
        {
            switch (g_op)
            {
            case '+': g_acc += cur; break;
            case '-': g_acc -= cur; break;
            case '*': g_acc *= cur; break;
            case '/': g_acc = (cur != 0) ? g_acc / cur : 0; break;
            }
            g_entering = false;
        }
        else
            g_acc = cur;
        g_op = 0;
        snprintf(g_buf, sizeof(g_buf), "%g", g_acc);
        break;
    }
    case '/':
    case '*':
    case '+':
    case '-':
    {
        double cur = atof(g_buf);
        if (g_op && g_entering)
        {
            switch (g_op)
            {
            case '+': g_acc += cur; break;
            case '-': g_acc -= cur; break;
            case '*': g_acc *= cur; break;
            case '/': g_acc = (cur != 0) ? g_acc / cur : 0; break;
            }
            snprintf(g_buf, sizeof(g_buf), "%g", g_acc);
        }
        else
            g_acc = cur;
        g_op = k[0];
        g_entering = false;
        break;
    }
    default: // 数字或 .
    {
        if (!g_entering)
        {
            if (strcmp(k, ".") == 0)
                snprintf(g_buf, sizeof(g_buf), "0.");
            else
                snprintf(g_buf, sizeof(g_buf), "%s", k);
            g_entering = true;
        }
        else
        {
            size_t n = strlen(g_buf);
            if (n < 20)
            {
                if (strcmp(k, ".") == 0)
                {
                    if (strchr(g_buf, '.') == NULL)
                    {
                        g_buf[n] = '.';
                        g_buf[n + 1] = 0;
                    }
                }
                else
                {
                    if (strcmp(g_buf, "0") == 0 && strcmp(k, "0") == 0)
                        break;
                    g_buf[n] = k[0];
                    g_buf[n + 1] = 0;
                }
            }
        }
        break;
    }
    }
    refresh_disp();
}

static void refresh_disp(void)
{
    lv_label_set_text(g_disp, g_buf[0] ? g_buf : "0");
}

static void build_keypad(lv_obj_t *parent)
{
    g_disp = lv_label_create(parent);
    lv_obj_set_width(g_disp, 296);
    lv_obj_set_pos(g_disp, 6, 10);
    lv_obj_set_style_text_align(g_disp, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(g_disp, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_disp, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_disp, "0");

    const int bw = 74, bh = 30, gx = 4, gy = 3;
    for (int i = 0; i < KEYS_N; i++)
    {
        int x = 6 + kCols[i] * (bw + gx);
        int y = 60 + kRows[i] * (bh + gy);
        lv_obj_t *b = lv_btn_create(parent);
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, x, y);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(b, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(b, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(l, kKeys[i]);
        lv_obj_center(l);
        g_keys[i] = b;
    }
    // 初始光标
    if (g_keys[0] != NULL)
        lv_obj_set_style_bg_color(g_keys[0], lv_color_hex(0x2A4A68), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void move_cur(int delta)
{
    lv_obj_set_style_bg_color(g_keys[g_cur], lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
    g_cur = (g_cur + delta + KEYS_N) % KEYS_N;
    lv_obj_set_style_bg_color(g_keys[g_cur], lv_color_hex(0x2A4A68), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (g_left > 0)
    {
        g_left--;
        move_cur(-1);
    }
    if (g_right > 0)
    {
        g_right--;
        move_cur(1);
    }
    if (g_enter > 0)
    {
        g_enter--;
        key_press(g_cur);
    }
    if (g_rec > 0)
    {
        g_rec--;
        key_press(8); // ⌫
    }
}

extern "C" const lios_app_info_t lios_app_info = {
    "calc", "计算器", "算", 40, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    g_left = g_right = g_enter = g_rec = 0;
    g_cur = 0;
    g_acc = 0; g_op = 0; g_entering = false;
    snprintf(g_buf, sizeof(g_buf), "0");
    build_keypad(parent);
    if (g_timer == NULL)
        g_timer = lv_timer_create(tick_cb, 30, NULL);
    return parent;
}

extern "C" void lios_app_destroy(void)
{
    if (g_timer != NULL)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
    g_root = NULL;
    for (int i = 0; i < KEYS_N; i++)
        g_keys[i] = NULL;
    g_disp = NULL;
}

extern "C" void lios_app_key(int key, int event)
{
    if (key == LIOS_KEY_LEFT && event == LIOS_EV_PRESS)
    {
        if (g_left < 4)
            g_left++;
    }
    else if (key == LIOS_KEY_RIGHT && event == LIOS_EV_PRESS)
    {
        if (g_right < 4)
            g_right++;
    }
    else if (key == LIOS_KEY_REC && event == LIOS_EV_PRESS)
    {
        if (g_rec < 4)
            g_rec++;
    }
    else if (key == LIOS_KEY_ENTER)
    {
        if (event == LIOS_EV_PRESS)
        {
            g_enter_down = true;
            g_enter_ms = lv_tick_get();
            g_enter_long = false;
        }
        else if (event == LIOS_EV_RELEASE)
        {
            if (g_enter_down)
            {
                g_enter_down = false;
                if (!g_enter_long && g_enter < 4)
                    g_enter++;
            }
        }
    }
}
