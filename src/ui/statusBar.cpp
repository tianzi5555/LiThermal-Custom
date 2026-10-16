// 顶部状态信息条
//
// 布局（320x240 屏）：
//   左上角小块  ： 高 36.5  低 21.0        ← 画面里飘的那份不动，这里只是复制一份
//   右上角两行  ： CPU 45.2° 12% 1008M     ← CPU 温度（内核支持后才有）/ 占用 / 频率
//                  模组 55%                ← 热成像模组(4117)自己的 CPU 占用（它的温度接口没暴露）
//
// 显隐规则：
//   * 菜单展开时（右上会弹电池卡片）整块隐藏
//   * 打开 SH 终端时也隐藏（否则会挡住终端页上方）
//   * 其余时候常显，并每秒抬到最前
//
// 数据来源：
//   CPU 占用/频率/温度 ：/proc/stat、scaling_cur_freq、/sys/class/thermal/thermal_zone0/temp
//   模组占用           ：cameraUtils.moduleCpu（由主循环节流从 4117 的 ISAPI 读）
//   最高/最低温度      ：cameraUtils.lastResult

#include "my_main.h"
#include "statusBar.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define SB_H 38
#define SB_BOX_W 172
#define SB_OPA 204 // ≈80%

static lv_obj_t *sb_box = NULL;   // 右上两块
static lv_obj_t *sb_l1 = NULL;
static lv_obj_t *sb_l2 = NULL;
static lv_obj_t *sb_mm = NULL;    // 左上最高/最低
static lv_obj_t *sb_mm_box = NULL;

static unsigned long long g_prev_total = 0, g_prev_idle = 0;

// ---------------- CPU 占用 ----------------
static int read_cpu_percent(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    unsigned long long v[10] = {0};
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n < 4) return -1;
    unsigned long long total = 0;
    for (int i = 0; i < n; i++) total += v[i];
    unsigned long long idle = v[3] + v[4];
    if (g_prev_total == 0) { g_prev_total = total; g_prev_idle = idle; return -1; }
    unsigned long long dt = total - g_prev_total, di = idle - g_prev_idle;
    g_prev_total = total; g_prev_idle = idle;
    if (dt == 0) return -1;
    int pct = (int)((dt - di) * 100 / dt);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ---------------- CPU 频率（MHz） ----------------
static int read_cpu_mhz(void)
{
    const char *paths[] = {
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",
        NULL};
    for (int i = 0; paths[i]; i++)
    {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1 && khz > 0) { fclose(f); return (int)(khz / 1000); }
        fclose(f);
    }
    return -1;
}

// ---------------- CPU 温度（0.1 度；没有驱动就返回 -1000） ----------------
static int read_cpu_temp_x10(void)
{
    const char *paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/devices/virtual/thermal/thermal_zone0/temp",
        NULL};
    for (int i = 0; paths[i]; i++)
    {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        long mc = 0;
        int ok = (fscanf(f, "%ld", &mc) == 1);
        fclose(f);
        if (!ok || mc <= -100000) return -1000;
        return (int)(mc / 100);
    }
    return -1000;
}

// ---------------- 定时刷新 ----------------
static void sb_update(lv_timer_t *t)
{
    (void)t;
    if (!sb_box || !lv_obj_is_valid(sb_box))
        return;

    bool hide = (current_mode == MODE_MAINMENU) || menu_terminal_is_open();
    if (hide)
    {
        lv_obj_add_flag(sb_box, LV_OBJ_FLAG_HIDDEN);
        if (sb_mm_box) lv_obj_add_flag(sb_mm_box, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(sb_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(sb_box);
        if (sb_mm_box)
        {
            lv_obj_clear_flag(sb_mm_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(sb_mm_box);
        }
    }

    // 第 1 行：CPU 温度 / 占用 / 频率
    int cpu = read_cpu_percent();
    int mhz = read_cpu_mhz();
    int t10 = read_cpu_temp_x10();
    char ts[16];
    if (t10 <= -1000) snprintf(ts, sizeof(ts), "--");
    else              snprintf(ts, sizeof(ts), "%d.%d", t10 / 10, t10 % 10);

    char l1[64];
    if (cpu < 0 && mhz <= 0)      snprintf(l1, sizeof(l1), "CPU %s° --", ts);
    else if (cpu < 0)             snprintf(l1, sizeof(l1), "CPU %s° -- %dM", ts, mhz);
    else if (mhz <= 0)            snprintf(l1, sizeof(l1), "CPU %s° %d%%", ts, cpu);
    else                          snprintf(l1, sizeof(l1), "CPU %s° %d%% %dM", ts, cpu, mhz);
    lv_label_set_text(sb_l1, l1);

    // 第 2 行：热成像模组自己的占用
    char l2[48];
    if (cameraUtils.moduleCpu >= 0 && cameraUtils.moduleMem >= 0)
        snprintf(l2, sizeof(l2), "模组 %d%% %d%%", cameraUtils.moduleCpu, cameraUtils.moduleMem);
    else if (cameraUtils.moduleCpu >= 0)
        snprintf(l2, sizeof(l2), "模组 %d%%", cameraUtils.moduleCpu);
    else
        snprintf(l2, sizeof(l2), "模组 --");
    lv_label_set_text(sb_l2, l2);

    // 左上：最高/最低温度（画面里飘的那份不动）
    if (sb_mm)
    {
        char mm[48];
        snprintf(mm, sizeof(mm), "高%.1f 低%.1f",
                 cameraUtils.lastResult.maxTemperature, cameraUtils.lastResult.minTemperature);
        lv_label_set_text(sb_mm, mm);
    }
}

void ui_statusbar_create(void)
{
    if (sb_box && lv_obj_is_valid(sb_box))
        return;

    // ---- 右上：两行 ----
    sb_box = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(sb_box, SB_BOX_W, SB_H);
    lv_obj_align(sb_box, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_clear_flag(sb_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(sb_box, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb_box, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sb_box, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sb_box, SB_OPA, LV_PART_MAIN | LV_STATE_DEFAULT);

    sb_l1 = lv_label_create(sb_box);
    lv_obj_set_pos(sb_l1, 0, 0);
    lv_label_set_text(sb_l1, "CPU --° -- --M");
    lv_obj_set_style_text_font(sb_l1, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sb_l1, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);

    sb_l2 = lv_label_create(sb_box);
    lv_obj_set_pos(sb_l2, 0, 16);
    lv_label_set_text(sb_l2, "模组 --");
    lv_obj_set_style_text_font(sb_l2, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sb_l2, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);

    // ---- 左上：最高/最低 ----
    sb_mm_box = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(sb_mm_box, 130, 20);
    lv_obj_align(sb_mm_box, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_clear_flag(sb_mm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(sb_mm_box, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb_mm_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb_mm_box, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sb_mm_box, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sb_mm_box, SB_OPA, LV_PART_MAIN | LV_STATE_DEFAULT);

    sb_mm = lv_label_create(sb_mm_box);
    lv_obj_set_pos(sb_mm, 0, 0);
    lv_label_set_text(sb_mm, "高-- 低--");
    lv_obj_set_style_text_font(sb_mm, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sb_mm, lv_color_hex(0xFFD479), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_timer_create(sb_update, 1000, NULL);
}
