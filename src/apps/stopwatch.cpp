// LiOS 秒表（插件）
// 确认键 = 开始/暂停；录像键 = 归零；运行时录像键 = 记圈（显示最近几圈）

#include "lios_app.h"
#include "my_main.h"

#include <stdio.h>
#include <string.h>

#define LAPS_MAX 8

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_time = NULL, *g_state = NULL, *g_laps = NULL;
static lv_timer_t *g_timer = NULL;

static bool g_running = false;
static uint32_t g_start_ms = 0;
static uint32_t g_paused_ms = 0;
static double g_elapsed = 0.0; // 秒
static char g_lapstr[LAPS_MAX * 12];
static int g_laps_n = 0;
static double g_lap_times[LAPS_MAX];

static int g_enter = 0, g_rec = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static bool g_enter_long = false;

static void fmt(double sec, char *out, size_t n)
{
    int m = (int)sec / 60;
    int s = (int)sec % 60;
    int cs = (int)((sec - (int)sec) * 100);
    snprintf(out, n, "%02d:%02d.%02d", m, s, cs);
}

static void refresh(void)
{
    char b[48];
    fmt(g_elapsed, b, sizeof(b));
    lv_label_set_text(g_time, b);
    lv_label_set_text(g_state, g_running ? "运行中" : (g_elapsed > 0 ? "已暂停" : "就绪"));
    lv_label_set_text(g_laps, g_lapstr[0] ? g_lapstr : "（无圈）");
}

static void add_lap(void)
{
    if (g_laps_n < LAPS_MAX)
    {
        g_lap_times[g_laps_n++] = g_elapsed;
        char b[12];
        char tmp[LAPS_MAX * 12] = "";
        for (int i = g_laps_n - 1; i >= 0; i--)
        {
            fmt(g_lap_times[i], b, sizeof(b));
            snprintf(tmp + strlen(tmp), sizeof(tmp) - strlen(tmp), "第%d圈 %s\n", g_laps_n - i, b);
        }
        snprintf(g_lapstr, sizeof(g_lapstr), "%s", tmp);
    }
    else
    {
        // 满了就覆盖旧圈
        for (int i = 0; i < LAPS_MAX - 1; i++)
            g_lap_times[i] = g_lap_times[i + 1];
        g_lap_times[LAPS_MAX - 1] = g_elapsed;
        char tmp[LAPS_MAX * 12] = "";
        char b[12];
        for (int i = LAPS_MAX - 1; i >= 0; i--)
        {
            fmt(g_lap_times[i], b, sizeof(b));
            snprintf(tmp + strlen(tmp), sizeof(tmp) - strlen(tmp), "圈%d %s\n", LAPS_MAX - i, b);
        }
        snprintf(g_lapstr, sizeof(g_lapstr), "%s", tmp);
    }
    refresh();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();

    if (g_running)
    {
        g_elapsed = g_paused_ms / 1000.0 + (now - g_start_ms) / 1000.0;
        refresh();
    }

    if (g_enter > 0)
    {
        g_enter--;
        if (g_running)
        {
            g_running = false;
            g_paused_ms += (now - g_start_ms);
            g_elapsed = g_paused_ms / 1000.0;
        }
        else
        {
            g_running = true;
            g_start_ms = now;
            if (g_elapsed <= 0)
                g_paused_ms = 0;
        }
        refresh();
    }
    if (g_rec > 0)
    {
        g_rec--;
        if (g_running)
            add_lap();
        else
        {
            g_elapsed = 0;
            g_paused_ms = 0;
            g_laps_n = 0;
            g_lapstr[0] = 0;
            refresh();
        }
    }
}

extern "C" const lios_app_info_t lios_app_info = {
    "stopwatch", "秒表", "表", 42, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    g_running = false;
    g_elapsed = 0;
    g_paused_ms = 0;
    g_laps_n = 0;
    g_lapstr[0] = 0;
    g_enter = g_rec = 0;

    g_time = lv_label_create(parent);
    lv_obj_set_style_text_font(g_time, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_time, lv_color_hex(0x7DF0A0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(g_time, LV_ALIGN_TOP_MID, 0, 50);
    lv_label_set_text(g_time, "00:00.00");

    g_state = lv_label_create(parent);
    lv_obj_set_style_text_font(g_state, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_state, lv_color_hex(0x64C8FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(g_state, LV_ALIGN_TOP_MID, 0, 80);
    lv_label_set_text(g_state, "就绪");

    g_laps = lv_label_create(parent);
    lv_obj_set_width(g_laps, 300);
    lv_obj_set_style_text_font(g_laps, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_laps, lv_color_hex(0xB4C4D0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(g_laps, 10, 110);
    lv_label_set_text(g_laps, "（无圈）");

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_style_text_font(hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(hint, "确认=开始/暂停   录像=归零/记圈");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

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
    g_time = g_state = g_laps = NULL;
}

extern "C" void lios_app_key(int key, int event)
{
    if (key == LIOS_KEY_REC && event == LIOS_EV_PRESS)
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
