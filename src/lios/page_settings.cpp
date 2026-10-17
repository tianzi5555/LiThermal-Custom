// LiOS 设置（仿安卓的分组列表）
//
// 分组与条目：
//   显示   亮度（若系统有 /sys/class/backlight 才显示；没有就如实写"不支持"）
//   温度   温度单位（C / F 切换）+ 当前 CPU 温度
//   应用   插件目录 / 已安装插件数量 / 重新扫描
//   关于   内核版本 / 桌面程序大小 / 权限说明
// 持久化：/mnt/UDISK/lios/settings.conf（temp_unit=c|f  brightness=NN）
//
// 纪律：钩子在 UI 线程且已持锁 → 绝不 LOCKLV、绝不做文件 I/O（读写配置、读 /proc、
//       写 sysfs 亮度全部在 worker 线程）。

#include "lios.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ROWS 12
#define SETTINGS_DIR "/mnt/UDISK/lios"
#define SETTINGS_FILE "/mnt/UDISK/lios/settings.conf"

typedef struct
{
    int temp_unit_f;   // 0 = C, 1 = F
    int brightness;    // -1 = 不支持
    int bright_max;
    char bright_path[160];
    int temp_x10;      // -1000 = 不可用
    int plugins;
    long lios_kb;
    char kernel[80];
    int valid;
} set_info_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static set_info_t s_info;
static bool s_ready = false;
static bool s_busy = false;
static bool s_reload = false;
static char s_status[128];

static lv_obj_t *s_title = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_rows[MAX_ROWS] = {0};
static lv_obj_t *s_row_lbl[MAX_ROWS] = {0};
static int s_row_n = 0;

static void ui_rebuild(void);
static void row_click_cb(lv_event_t *e);

// ---------------- worker
static void probe_backlight(set_info_t *t)
{
    t->brightness = -1;
    t->bright_max = 255;
    t->bright_path[0] = 0;
    DIR *d = opendir("/sys/class/backlight");
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        char p[200];
        snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness", e->d_name);
        FILE *f = fopen(p, "r");
        if (f == NULL)
            continue;
        long v = -1;
        if (fscanf(f, "%ld", &v) == 1)
        {
            t->brightness = (int)v;
            snprintf(t->bright_path, sizeof(t->bright_path), "%s", p);
        }
        fclose(f);
        snprintf(p, sizeof(p), "/sys/class/backlight/%s/max_brightness", e->d_name);
        f = fopen(p, "r");
        if (f != NULL)
        {
            long mv = 255;
            if (fscanf(f, "%ld", &mv) == 1 && mv > 0)
                t->bright_max = (int)mv;
            fclose(f);
        }
        break;
    }
    closedir(d);
}

static void load_settings(set_info_t *t)
{
    t->temp_unit_f = 0;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (f == NULL)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "temp_unit=", 10) == 0)
            t->temp_unit_f = (line[10] == 'f' || line[10] == 'F') ? 1 : 0;
    }
    fclose(f);
}

static void save_settings(const set_info_t *t)
{
    mkdir(SETTINGS_DIR, 0755);
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (f == NULL)
        return;
    fprintf(f, "temp_unit=%c\n", t->temp_unit_f ? 'f' : 'c');
    fclose(f);
}

static void worker_load(void)
{
    set_info_t t;
    memset(&t, 0, sizeof(t));
    load_settings(&t);
    probe_backlight(&t);

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

    snprintf(t.kernel, sizeof(t.kernel), "%s", "unknown");
    f = fopen("/proc/version", "r");
    if (f != NULL)
    {
        char buf[256];
        if (fgets(buf, sizeof(buf), f) != NULL)
        {
            // 只取前 60 字符，够看内核版本
            buf[60] = 0;
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

static void worker_save_and_apply(int set_brightness_delta)
{
    pthread_mutex_lock(&s_mtx);
    set_info_t t = s_info;
    pthread_mutex_unlock(&s_mtx);

    if (set_brightness_delta != 0 && t.bright_path[0] != 0)
    {
        int step = t.bright_max / 10;
        if (step < 1)
            step = 1;
        int v = t.brightness + set_brightness_delta * step;
        if (v < 1)
            v = 1;
        if (v > t.bright_max)
            v = t.bright_max;
        FILE *f = fopen(t.bright_path, "w");
        if (f != NULL)
        {
            fprintf(f, "%d", v);
            fclose(f);
            t.brightness = v;
        }
    }
    save_settings(&t);

    pthread_mutex_lock(&s_mtx);
    s_info = t;
    snprintf(s_status, sizeof(s_status), "已保存");
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
}

static void *settings_worker(void *)
{
    for (;;)
    {
        int job = 0, delta = 0;
        pthread_mutex_lock(&s_mtx);
        if (!s_busy)
        {
            if (s_reload)
            {
                s_reload = false;
                s_busy = true;
                job = 1;
            }
            else
            {
                s_busy = true;
                job = 2;
            }
        }
        pthread_mutex_unlock(&s_mtx);

        if (job == 1)
            worker_load();
        else if (job == 2)
        {
            worker_load();
            usleep(2000 * 1000);
        }
        else
            usleep(50 * 1000);
        (void)delta;
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

// 一次性动作线程（切换单位 / 调亮度）
static void *apply_thread(void *arg)
{
    int delta = (int)(intptr_t)arg;
    pthread_mutex_lock(&s_mtx);
    s_busy = true;
    pthread_mutex_unlock(&s_mtx);
    worker_save_and_apply(delta);
    return NULL;
}

// ---------------- UI
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

static void ui_rebuild(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list))
        return;
    row_pool();

    set_info_t t;
    pthread_mutex_lock(&s_mtx);
    t = s_info;
    pthread_mutex_unlock(&s_mtx);

    char b[MAX_ROWS][150];
    int n = 0;

    // 显示
    if (t.brightness >= 0)
        snprintf(b[n++], 150, "亮度        %d / %d   （确认加 / 录像减）", t.brightness, t.bright_max);
    else
        snprintf(b[n++], 150, "亮度        此系统没有 backlight 节点，不支持");
    // 温度
    if (t.temp_x10 <= -1000)
        snprintf(b[n++], 150, "CPU 温度    --");
    else if (t.temp_unit_f)
    {
        int f10 = (int)(t.temp_x10 * 9 / 5 + 320);
        snprintf(b[n++], 150, "CPU 温度    %d.%d F", f10 / 10, f10 % 10);
    }
    else
        snprintf(b[n++], 150, "CPU 温度    %d.%d C", t.temp_x10 / 10, t.temp_x10 % 10);
    snprintf(b[n++], 150, "温度单位    %s   （确认切换）", t.temp_unit_f ? "华氏 F" : "摄氏 C");
    // 应用
    snprintf(b[n++], 150, "插件目录    /mnt/UDISK/apps");
    snprintf(b[n++], 150, "已装插件    %d 个", t.plugins);
    // 关于
    if (t.lios_kb >= 0)
        snprintf(b[n++], 150, "桌面程序    lios  %ld KB", t.lios_kb);
    else
        snprintf(b[n++], 150, "桌面程序    lios");
    snprintf(b[n++], 150, "内核        %s", t.kernel);
    snprintf(b[n++], 150, "权限说明    只记录与展示，非强制沙箱");

    for (int i = 0; i < MAX_ROWS; i++)
    {
        if (s_rows[i] == NULL || !lv_obj_is_valid(s_rows[i]))
            continue;
        if (i < n)
        {
            lv_label_set_text(s_row_lbl[i], b[i]);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
        }
        else
        {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
        }
    }
    s_row_n = n;
    if (n > 0)
        lv_group_focus_obj(s_rows[0]);
}

static void row_click_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (row == 2) // 温度单位
    {
        pthread_mutex_lock(&s_mtx);
        s_info.temp_unit_f = s_info.temp_unit_f ? 0 : 1;
        pthread_mutex_unlock(&s_mtx);
        pthread_t t;
        if (pthread_create(&t, NULL, apply_thread, (void *)(intptr_t)0) == 0)
            pthread_detach(t);
        ui_rebuild();
    }
    else if (row == 0) // 亮度加
    {
        pthread_t t;
        if (pthread_create(&t, NULL, apply_thread, (void *)(intptr_t)1) == 0)
            pthread_detach(t);
    }
    else if (row == 4 || row == 5)
    {
        pthread_mutex_lock(&s_mtx);
        s_reload = true;
        pthread_mutex_unlock(&s_mtx);
        if (s_hint != NULL && lv_obj_is_valid(s_hint))
            lv_label_set_text(s_hint, "正在重新扫描插件目录…");
    }
}

static bool page_key(int key)
{
    if (key == LIOS_K_MENU)
        return false;
    if (key == LIOS_K_REC)
    {
        // 亮度减
        pthread_t t;
        if (pthread_create(&t, NULL, apply_thread, (void *)(intptr_t)-1) == 0)
            pthread_detach(t);
        return true;
    }
    if (key == LIOS_K_ENTER)
    {
        static uint32_t last = 0;
        uint32_t now = lv_tick_get();
        if (now - last < 200)
            return true;
        last = now;
        return true;
    }
    return false;
}

static void page_tick(void)
{
    static bool built = false;
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();

    pthread_mutex_lock(&s_mtx);
    bool ready = s_ready;
    char st[128];
    snprintf(st, sizeof(st), "%s", s_status);
    pthread_mutex_unlock(&s_mtx);

    if (ready && (!built || now - last > 2000))
    {
        built = true;
        last = now;
        ui_rebuild();
    }
    if (st[0] != 0)
    {
        pthread_mutex_lock(&s_mtx);
        s_status[0] = 0;
        pthread_mutex_unlock(&s_mtx);
        if (s_hint != NULL && lv_obj_is_valid(s_hint))
            lv_label_set_text(s_hint, st);
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
}

static const lios_page_hooks_t s_hooks = {page_key, page_tick, page_destroy};

void page_settings_create(lv_obj_t *parent)
{
    settings_worker_start();

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
    lv_label_set_text(s_hint, "读取中…   菜单=返回桌面");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    lios_page_set_hooks(&s_hooks);
    lios_log("设置：页面已建");
}
