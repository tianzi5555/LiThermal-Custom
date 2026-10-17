// LiOS 设置（仿安卓分组列表）
//
// 亮度更正（用户指出）：相机不走 /sys/class/backlight ✗，而是 /dev/disp 的 ioctl
//   （DISP_LCD_GET_BRIGHTNESS / DISP_LCD_SET_BRIGHTNESS）+ gamma_lut。
//   这里移植同一套，所以能像相机一样真调背光。
//
// 刷新更正（用户实测）：以前每次刷新都重建列表并 lv_group_focus_obj(第0行)
//   → 滚到哪都被拽回第一行。现在拆成 ui_build()（只建一次/设一次焦点）
//   与 ui_refresh()（只改文字，绝不碰焦点）。
//
// 纪律：钩子在 UI 线程且已持锁 → 绝不 LOCKLV、绝不做文件 I/O。

#include "lios.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
// sunxi_display2.h 需要这些类型先定义（相机 Backlight.cpp 里也是这么写的）
typedef uint32_t u32;
typedef int32_t s32;
typedef uint16_t u16;
typedef uint8_t u8;
#include <sunxi_display2.h>

#define MAX_ROWS 10
#define SETTINGS_DIR "/mnt/UDISK/liosdata"
#define SETTINGS_FILE "/mnt/UDISK/liosdata/settings.conf"

// ---------------------------------------------------------------- 背光
static int s_disp_fd = -1;
static unsigned long s_args[4];

static const unsigned char kGamma[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
    4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9,
    9, 9, 10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16,
    16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 23, 23, 24, 24,
    25, 26, 26, 27, 28, 28, 29, 30, 30, 31, 32, 32, 33, 34, 35, 35,
    36, 37, 38, 38, 39, 40, 41, 42, 42, 43, 44, 45, 46, 47, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 73, 74, 75, 76, 77, 78, 79, 80,
    81, 82, 84, 85, 86, 87, 88, 89, 91, 92, 93, 94, 95, 97, 98, 99,
    100, 102, 103, 104, 105, 107, 108, 109, 111, 112, 113, 115, 116, 117, 119, 120,
    121, 123, 124, 126, 127, 128, 130, 131, 133, 134, 136, 137, 139, 140, 142, 143,
    145, 146, 148, 149, 151, 152, 154, 155, 157, 158, 160, 162, 163, 165, 166, 168,
    170, 171, 173, 175, 176, 178, 180, 181, 183, 185, 186, 188, 190, 192, 193, 195,
    197, 199, 200, 202, 204, 206, 207, 209, 211, 213, 215, 217, 218, 220, 222, 224,
    226, 228, 230, 232, 233, 235, 237, 239, 241, 243, 245, 247, 249, 251, 253, 255};

static int bright_init(void)
{
    s_disp_fd = open("/dev/disp", O_RDWR, 0);
    return s_disp_fd > 0 ? 0 : -1;
}

static int bright_get(void)
{
    if (s_disp_fd <= 0)
        return -1;
    s_args[0] = 0;
    int raw = ioctl(s_disp_fd, DISP_LCD_GET_BRIGHTNESS, s_args);
    if (raw < 0)
        return -1;
    for (int i = 0; i < 256; i++)
        if (raw == kGamma[i])
            return i;
    return raw > 255 ? 255 : raw;
}

static int bright_set(int v)
{
    if (s_disp_fd <= 0)
        return -1;
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    s_args[0] = 0;
    s_args[1] = kGamma[v];
    s_args[2] = 0;
    ioctl(s_disp_fd, DISP_LCD_SET_BRIGHTNESS, s_args);
    return v;
}

// ---------------------------------------------------------------- 共享数据
typedef struct
{
    int temp_unit_f;
    int brightness;
    int temp_x10;
    int plugins;
    long lios_kb;
    char kernel[80];
    int valid;
} set_info_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static set_info_t s_info;
static bool s_ready = false;
static bool s_busy = false;
static char s_status[128];

static lv_obj_t *s_title = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_rows[MAX_ROWS] = {0};
static lv_obj_t *s_row_lbl[MAX_ROWS] = {0};
static int s_row_n = 0;
static bool s_built = false;
static uint32_t s_override_until = 0;

enum
{
    ROW_BRIGHT = 0,
    ROW_TEMP,
    ROW_UNIT,
    ROW_PLUGDIR,
    ROW_PLUGCNT,
    ROW_LIOSSIZE,
    ROW_KERNEL,
    ROW_PERMNOTE,
    ROW_CNT
};

static void ui_build(void);
static void ui_refresh(void);
static void row_click_cb(lv_event_t *e);

static void set_hint(const char *s)
{
    if (s_hint != NULL && lv_obj_is_valid(s_hint))
        lv_label_set_text(s_hint, s);
}

// ---------------------------------------------------------------- 配置 / 读数
static void load_settings(set_info_t *t)
{
    t->temp_unit_f = 0;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (f == NULL)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, "temp_unit=", 10) == 0)
            t->temp_unit_f = (line[10] == 'f' || line[10] == 'F') ? 1 : 0;
    fclose(f);
}

static void save_settings(set_info_t *t)
{
    mkdir(SETTINGS_DIR, 0755);
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (f == NULL)
    {
        snprintf(s_status, sizeof(s_status), "保存失败：写不了配置文件");
        return;
    }
    fprintf(f, "temp_unit=%c\n", t->temp_unit_f ? 'f' : 'c');
    fclose(f);
}

static void worker_load(void)
{
    set_info_t t;
    memset(&t, 0, sizeof(t));
    load_settings(&t);

    if (s_disp_fd <= 0)
        bright_init();
    t.brightness = bright_get();

    t.temp_x10 = -1000;
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f != NULL)
    {
        long mc = -1;
        if (fscanf(f, "%ld", &mc) == 1)
            t.temp_x10 = (int)(mc / 100);
        fclose(f);
    }
    t.plugins = lios_apps_total() - LIOS_APP_COUNT;
    if (t.plugins < 0)
        t.plugins = 0;

    t.lios_kb = -1;
    struct stat st;
    if (stat("/mnt/UDISK/lios", &st) == 0)
        t.lios_kb = (long)(st.st_size / 1024);

    snprintf(t.kernel, sizeof(t.kernel), "unknown");
    f = fopen("/proc/version", "r");
    if (f != NULL)
    {
        char buf[256];
        if (fgets(buf, sizeof(buf), f) != NULL)
        {
            buf[56] = 0;
            snprintf(t.kernel, sizeof(t.kernel), "%s", buf);
        }
        fclose(f);
    }
    t.valid = 1;

    pthread_mutex_lock(&s_mtx);
    s_info = t;
    s_ready = true;
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
}

// action: 1=亮度+  2=亮度-  3=切换单位  0=只保存
static void *settings_action(void *arg)
{
    int action = (int)(intptr_t)arg;

    pthread_mutex_lock(&s_mtx);
    s_busy = true;
    set_info_t t = s_info;
    pthread_mutex_unlock(&s_mtx);

    if (action == 1)
    {
        int step = 16;
        t.brightness = bright_set((t.brightness < 0 ? 128 : t.brightness) + step);
    }
    else if (action == 2)
    {
        int step = 16;
        t.brightness = bright_set((t.brightness < 0 ? 128 : t.brightness) - step);
    }
    else if (action == 3)
    {
        t.temp_unit_f = t.temp_unit_f ? 0 : 1;
        save_settings(&t);
    }

    pthread_mutex_lock(&s_mtx);
    s_info = t;
    snprintf(s_status, sizeof(s_status), action == 3 ? "已保存" : "亮度 %d", t.brightness);
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
    return NULL;
}

static void *settings_worker(void *)
{
    for (;;)
    {
        bool do_load = false;
        pthread_mutex_lock(&s_mtx);
        if (!s_busy)
        {
            s_busy = true;
            do_load = true;
        }
        pthread_mutex_unlock(&s_mtx);
        if (do_load)
        {
            worker_load();
            usleep(2000 * 1000); // 2 秒刷新读数
        }
        else
            usleep(50 * 1000);
    }
    return NULL;
}

static void settings_worker_start(void)
{
    static bool started = false;
    if (started)
        return;
    started = true;
    pthread_t t;
    if (pthread_create(&t, NULL, settings_worker, NULL) == 0)
        pthread_detach(t);
    else
        lios_log("设置：worker 创建失败");
}

// ---------------------------------------------------------------- UI
static void row_pool(void)
{
    if (s_rows[0] != NULL)
        return;
    for (int i = 0; i < MAX_ROWS; i++)
    {
        lv_obj_t *r = lv_btn_create(s_list);
        lv_obj_set_size(r, 316, 22);
        lv_obj_set_pos(r, 0, i * 22);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_radius(r, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(r, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(r, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *l = lv_label_create(r);
        lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, row_click_cb, LV_EVENT_CLICKED, NULL);
        s_rows[i] = r;
        s_row_lbl[i] = l;
    }
}

static void ui_refresh(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list) || s_rows[0] == NULL)
        return;

    set_info_t t;
    pthread_mutex_lock(&s_mtx);
    t = s_info;
    pthread_mutex_unlock(&s_mtx);

    char b[ROW_CNT][150];
    if (t.brightness >= 0)
        snprintf(b[ROW_BRIGHT], 150, "亮度        %d / 255   （确认+  录像-）", t.brightness);
    else
        snprintf(b[ROW_BRIGHT], 150, "亮度        背光读取失败（/dev/disp 打不开）");
    if (t.temp_x10 <= -1000)
        snprintf(b[ROW_TEMP], 150, "CPU 温度    --");
    else if (t.temp_unit_f)
    {
        int f10 = (int)(t.temp_x10 * 9 / 5 + 320);
        snprintf(b[ROW_TEMP], 150, "CPU 温度    %d.%d F", f10 / 10, f10 % 10);
    }
    else
        snprintf(b[ROW_TEMP], 150, "CPU 温度    %d.%d C", t.temp_x10 / 10, t.temp_x10 % 10);
    snprintf(b[ROW_UNIT], 150, "温度单位    %s   （确认切换）", t.temp_unit_f ? "华氏 F" : "摄氏 C");
    snprintf(b[ROW_PLUGDIR], 150, "插件目录    /mnt/UDISK/apps");
    snprintf(b[ROW_PLUGCNT], 150, "已装插件    %d 个", t.plugins);
    if (t.lios_kb >= 0)
        snprintf(b[ROW_LIOSSIZE], 150, "桌面程序    lios  %ld KB", t.lios_kb);
    else
        snprintf(b[ROW_LIOSSIZE], 150, "桌面程序    lios");
    snprintf(b[ROW_KERNEL], 150, "内核        %s", t.kernel);
    snprintf(b[ROW_PERMNOTE], 150, "权限说明    只记录与展示，非强制沙箱");

    for (int i = 0; i < ROW_CNT && i < MAX_ROWS; i++)
        if (s_row_lbl[i] != NULL && lv_obj_is_valid(s_row_lbl[i]))
            lv_label_set_text(s_row_lbl[i], b[i]);
    s_row_n = ROW_CNT;
}

static void ui_build(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list))
        return;
    row_pool();
    ui_refresh();
    for (int i = 0; i < MAX_ROWS; i++)
    {
        if (s_rows[i] == NULL || !lv_obj_is_valid(s_rows[i]))
            continue;
        if (i < s_row_n)
        {
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
        }
        else
        {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
        }
    }
    if (s_row_n > 0)
        lv_group_focus_obj(s_rows[0]);
    set_hint("确认=调节/切换   录像=亮度减   菜单=回桌面");
    s_built = true;
}

static void spawn_action(int action)
{
    pthread_t t;
    if (pthread_create(&t, NULL, settings_action, (void *)(intptr_t)action) == 0)
        pthread_detach(t);
}

static void row_click_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (row == ROW_BRIGHT || row == ROW_TEMP)
        spawn_action(1); // 加亮度
    else if (row == ROW_UNIT)
        spawn_action(3);
    else
        set_hint("该项只是显示（确认键无动作）");
}

static bool page_key(int key)
{
    if (key == LIOS_K_MENU)
        return false;
    if (key == LIOS_K_REC)
    {
        spawn_action(2); // 亮度减
        return true;
    }
    if (key == LIOS_K_ENTER)
    {
        static uint32_t last = 0;
        uint32_t now = lv_tick_get();
        if (now - last < 200)
            return true;
        last = now;
        return true; // 点击交给 LVGL 派发给焦点行
    }
    return false;
}

static void page_tick(void)
{
    static uint32_t last_refresh = 0;
    uint32_t now = lv_tick_get();

    pthread_mutex_lock(&s_mtx);
    bool ready = s_ready;
    char st[128];
    snprintf(st, sizeof(st), "%s", s_status);
    if (st[0] != 0)
        s_status[0] = 0;
    pthread_mutex_unlock(&s_mtx);

    if (!ready)
    {
        set_hint("读取中…   菜单=回桌面");
        return;
    }
    if (!s_built)
    {
        ui_build();
        last_refresh = now;
    }
    else if (now - last_refresh > 2000)
    {
        ui_refresh(); // 只改文字，不碰焦点 ✓
        last_refresh = now;
    }

    if (st[0] != 0)
    {
        set_hint(st);
        s_override_until = now + 2500;
    }
    else if (s_override_until != 0 && now > s_override_until)
    {
        s_override_until = 0;
        set_hint("确认=调节/切换   录像=亮度减   菜单=回桌面");
    }
}

static void page_destroy(void)
{
    s_title = NULL;
    s_list = NULL;
    s_hint = NULL;
    for (int i = 0; i < MAX_ROWS; i++)
    {
        s_rows[i] = NULL;
        s_row_lbl[i] = NULL;
    }
    s_row_n = 0;
    s_built = false;
    s_override_until = 0;
}

static const lios_page_hooks_t s_hooks = {page_key, page_tick, page_destroy};

void page_settings_create(lv_obj_t *parent)
{
    settings_worker_start();
    s_built = false;
    s_override_until = 0;

    s_title = lv_label_create(parent);
    lv_obj_set_style_text_font(s_title, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_title, "设置");
    lv_obj_set_pos(s_title, 4, 4);

    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, 316, 240 - 24 - 16);
    lv_obj_set_pos(s_list, 2, 24);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_hint, "读取中…   菜单=回桌面");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    lios_page_set_hooks(&s_hooks);
    lios_log("设置：页面已建");
}
