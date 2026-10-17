// LiOS 单位转换器（插件）
// 预设换算对：滚轮左右切换；波轮按下 = 输入值 +1；录像键 = 输入值 -1；
// 长按波轮 = 输入值归 1。

#include "lios_app.h"
#include "my_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_UNITS 12

static const char *kNames[N_UNITS] = {
    "米 → 千米", "千米 → 米",
    "公斤 → 斤", "斤 → 公斤",
    "摄氏 → 华氏", "华氏 → 摄氏",
    "米/秒 → 千米/时", "千米/时 → 米/秒",
    "升 → 毫升", "毫升 → 升",
    "千米/时 → 节", "节 → 千米/时"
};

// 换算函数：返回 y = f(x)
static double f_unit(int idx, double x)
{
    switch (idx)
    {
    case 0: return x / 1000.0;            // m -> km
    case 1: return x * 1000.0;            // km -> m
    case 2: return x * 2.0;               // kg -> 斤
    case 3: return x / 2.0;               // 斤 -> kg
    case 4: return x * 9.0 / 5.0 + 32.0;  // C -> F
    case 5: return (x - 32.0) * 5.0 / 9.0; // F -> C
    case 6: return x * 3.6;               // m/s -> km/h
    case 7: return x / 3.6;               // km/h -> m/s
    case 8: return x * 1000.0;            // L -> mL
    case 9: return x / 1000.0;            // mL -> L
    case 10: return x / 1.852;            // km/h -> 节
    case 11: return x * 1.852;            // 节 -> km/h
    }
    return x;
}

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_name = NULL, *g_val = NULL, *g_res = NULL;
static lv_timer_t *g_timer = NULL;

static int g_sel = 0;
static double g_value = 1.0;
static int g_left = 0, g_right = 0, g_up = 0, g_down = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static bool g_enter_long = false;

static void refresh(void)
{
    double r = f_unit(g_sel, g_value);
    char b[80];
    snprintf(b, sizeof(b), "%s", kNames[g_sel]);
    lv_label_set_text(g_name, b);
    snprintf(b, sizeof(b), "输入：%g", g_value);
    lv_label_set_text(g_val, b);
    if (r == (long)r && r > -1e9 && r < 1e9)
        snprintf(b, sizeof(b), "结果：%ld", (long)r);
    else
        snprintf(b, sizeof(b), "结果：%.6g", r);
    lv_label_set_text(g_res, b);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    if (g_enter_down && !g_enter_long && (now - g_enter_ms >= 1500))
    {
        g_enter_long = true;
        g_value = 1.0; // 长按归 1
        refresh();
    }
    if (g_left > 0)
    {
        g_left--;
        g_sel = (g_sel + N_UNITS - 1) % N_UNITS;
        refresh();
    }
    if (g_right > 0)
    {
        g_right--;
        g_sel = (g_sel + 1) % N_UNITS;
        refresh();
    }
    if (g_up > 0)
    {
        g_up--;
        g_value += 1.0;
        refresh();
    }
    if (g_down > 0)
    {
        g_down--;
        g_value -= 1.0;
        if (g_value < 0)
            g_value = 0;
        refresh();
    }
}

extern "C" const lios_app_info_t lios_app_info = {
    "unitconv", "单位转换", "换", 41, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    g_sel = 0;
    g_value = 1.0;
    g_left = g_right = g_up = g_down = 0;

    g_name = lv_label_create(parent);
    lv_obj_set_width(g_name, 300);
    lv_obj_set_style_text_font(g_name, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_name, lv_color_hex(0x64C8FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(g_name, LV_ALIGN_TOP_MID, 0, 40);

    g_val = lv_label_create(parent);
    lv_obj_set_width(g_val, 300);
    lv_obj_set_style_text_font(g_val, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_val, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(g_val, LV_ALIGN_TOP_MID, 0, 90);

    g_res = lv_label_create(parent);
    lv_obj_set_width(g_res, 300);
    lv_obj_set_style_text_font(g_res, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_res, lv_color_hex(0x7DF0A0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(g_res, LV_ALIGN_TOP_MID, 0, 140);

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_style_text_font(hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(hint, "滚轮=换单位   确认=输入+1   录像=输入-1   长按确认=归1");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    refresh();
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
    g_name = g_val = g_res = NULL;
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
        if (g_down < 4)
            g_down++;
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
                if (!g_enter_long && g_up < 4)
                    g_up++;
            }
        }
    }
}
