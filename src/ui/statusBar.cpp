// 顶部状态条（精简版 v2）
//
// 只显示：CPU 温度 + CPU 占用
//   * 透明度：LV_OPA_50（照抄原项目中心温度标签 src/ui/crosshairs.cpp 的样式）
//   * 宽度：内容自适应，贴着文字，不留空
//   * 文字全用 ASCII（避免缺字形显示成方框；温度用 C 表示摄氏度）
//   * 位置：右上角；菜单展开 / 打开 SH 终端时隐藏
//
// 性能：定时器只做两件极轻的事（读 /proc/stat 求差、读一次温度文件），
//       都是内存伪文件，开销 < 0.1ms；且不再每秒 move_foreground（避免整层重绘）。

#include "my_main.h"
#include "statusBar.h"

#include <stdio.h>
#include <string.h>

#define SB_OPA LV_OPA_50

static lv_obj_t *sb_box = NULL;
static lv_obj_t *sb_l1 = NULL;
static unsigned long long g_prev_total = 0, g_prev_idle = 0;

static int read_cpu_percent(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
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
        if (!ok) return -1000;
        return (int)(mc / 100);
    }
    return -1000;
}

static void sb_update(lv_timer_t *t)
{
    (void)t;
    if (!sb_box || !lv_obj_is_valid(sb_box))
        return;

    static bool was_hidden = false;
    bool hide = (current_mode == MODE_MAINMENU) || menu_terminal_is_open();
    if (hide)
    {
        if (!was_hidden) { lv_obj_add_flag(sb_box, LV_OBJ_FLAG_HIDDEN); was_hidden = true; }
        return;
    }
    if (was_hidden)
    {
        lv_obj_clear_flag(sb_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(sb_box);
        was_hidden = false;
    }

    int cpu = read_cpu_percent();
    int t10 = read_cpu_temp_x10();
    char ts[16];
    if (t10 <= -1000) snprintf(ts, sizeof(ts), "--");
    else              snprintf(ts, sizeof(ts), "%d.%d", t10 / 10, t10 % 10);

    char l1[40];
    if (cpu < 0) snprintf(l1, sizeof(l1), "CPU %sC", ts);
    else         snprintf(l1, sizeof(l1), "CPU %sC %d%%", ts, cpu);
    lv_label_set_text(sb_l1, l1);
}

void ui_statusbar_create(void)
{
    if (sb_box && lv_obj_is_valid(sb_box))
        return;

    sb_box = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(sb_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(sb_box, LV_ALIGN_TOP_RIGHT, -1, 0);
    lv_obj_clear_flag(sb_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(sb_box, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_hor(sb_box, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_ver(sb_box, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sb_box, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sb_box, SB_OPA, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(sb_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    sb_l1 = lv_label_create(sb_box);
    lv_label_set_text(sb_l1, "CPU --C --");
    lv_obj_set_style_text_font(sb_l1, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sb_l1, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_timer_create(sb_update, 2000, NULL);
}
