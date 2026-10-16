// 主界面顶部的半透明状态条：显示 CPU 占用 / 内存占用
//
// 数据来源（都是现成的，不需要新驱动）：
//   CPU  : /proc/stat 第一行（两次采样求差）
//   内存 : /proc/meminfo 的 MemTotal / MemAvailable
//
// 尺寸：左上角 244×24（右上角留给电池卡片 x=250 宽 56+），
//       背景不透明度约 80%（204/255），不影响看画面。
// 更新：LVGL 定时器每 1 秒一次（跑在 LVGL 线程里，不需要另外加锁）。

#include "my_main.h"
#include "statusBar.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 满宽（按屏幕宽度取），菜单展开时整条隐藏
#define SB_W 320
#define SB_H 24
#define SB_OPA 204 // ≈80%

static lv_obj_t *sb_bar = NULL;
static lv_obj_t *sb_lbl = NULL;

static unsigned long long g_prev_total = 0, g_prev_idle = 0;

// 读 /proc/stat 第一行，算 CPU 占用百分比；第一次返回 -1
static int read_cpu_percent(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    char line[256];
    if (!fgets(line, sizeof(line), f))
    {
        fclose(f);
        return -1;
    }
    fclose(f);

    // cpu  user nice system idle iowait irq softirq steal guest guest_nice
    unsigned long long v[10] = {0};
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n < 4)
        return -1;

    unsigned long long total = 0;
    for (int i = 0; i < n; i++)
        total += v[i];
    unsigned long long idle = v[3] + v[4]; // idle + iowait

    if (g_prev_total == 0)
    {
        g_prev_total = total;
        g_prev_idle = idle;
        return -1;
    }

    unsigned long long dt = total - g_prev_total;
    unsigned long long di = idle - g_prev_idle;
    g_prev_total = total;
    g_prev_idle = idle;
    if (dt == 0)
        return -1;

    int pct = (int)((dt - di) * 100 / dt);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}

// 读 /proc/meminfo 算内存占用百分比
static int read_mem_percent(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char key[64];
    unsigned long val;
    char unit[16];
    unsigned long total = 0, avail = 0, free_kb = 0;
    while (fscanf(f, "%63s %lu %15s", key, &val, unit) == 3)
    {
        if (strcmp(key, "MemTotal:") == 0)
            total = val;
        else if (strcmp(key, "MemAvailable:") == 0)
            avail = val;
        else if (strcmp(key, "MemFree:") == 0)
            free_kb = val;
    }
    fclose(f);
    if (total == 0)
        return -1;
    if (avail == 0)
        avail = free_kb; // 老内核没有 MemAvailable 时退化
    int pct = (int)((total - avail) * 100 / total);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}


// 读 CPU 温度：/sys/class/thermal/thermal_zone0/temp（毫摄氏度）
// 这台相机目前内核没编 thermal 框架，所以通常读不到，返回 -1000 表示无数据。
static int read_cpu_temp_x10(void)
{
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f)
        return -1000;
    long mc = 0;
    if (fscanf(f, "%ld", &mc) != 1)
        mc = -1000000;
    fclose(f);
    if (mc <= -100000)
        return -1000;
    return (int)(mc / 100); // 0.1 度
}

static void sb_update(lv_timer_t *t)
{
    (void)t;
    if (!sb_bar || !lv_obj_is_valid(sb_bar) || !sb_lbl || !lv_obj_is_valid(sb_lbl))
        return;

    // 菜单展开时（右上角会弹出电池卡片）整条隐藏；其余时候显示并抬到最前面
    if (current_mode == MODE_MAINMENU)
    {
        lv_obj_add_flag(sb_bar, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(sb_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(sb_bar);
    }

    int cpu = read_cpu_percent();
    int mem = read_mem_percent();
    int t10 = read_cpu_temp_x10();
    char buf[80];
    char cpus[16], mems[16], temps[20];
    if (cpu < 0)
        snprintf(cpus, sizeof(cpus), "--%%");
    else
        snprintf(cpus, sizeof(cpus), "%d%%", cpu);
    snprintf(mems, sizeof(mems), "%d%%", mem);
    if (t10 <= -1000)
        snprintf(temps, sizeof(temps), "--");
    else
        snprintf(temps, sizeof(temps), "%d.%dC", t10 / 10, t10 % 10);
    snprintf(buf, sizeof(buf), "CPU %s  %s  MEM %s", cpus, temps, mems);
    lv_label_set_text(sb_lbl, buf);
}

void ui_statusbar_create(void)
{
    if (sb_bar && lv_obj_is_valid(sb_bar))
        return;

    // 放到 lv_layer_sys：这是最上层，摄像头画面、相册、设置页都能看到它
    sb_bar = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(sb_bar, lv_disp_get_hor_res(NULL), SB_H);
    lv_obj_set_pos(sb_bar, 0, 0);
    lv_obj_clear_flag(sb_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(sb_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb_bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sb_bar, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sb_bar, SB_OPA, LV_PART_MAIN | LV_STATE_DEFAULT);

    sb_lbl = lv_label_create(sb_bar);
    lv_label_set_text(sb_lbl, "CPU --  --  MEM --%");
    lv_obj_set_style_text_font(sb_lbl, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sb_lbl, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(sb_lbl, LV_ALIGN_LEFT_MID, 2, 0);

    lv_timer_create(sb_update, 1000, NULL);
}
