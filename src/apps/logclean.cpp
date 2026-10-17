// LiOS 日志清理（插件）
// 扫描 /tmp 与 /mnt/UDISK 下的 .log 文件，按来源显示，可查看内容 / 删除

#include "lios_app.h"
#include "my_main.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_LOGS 64
#define MAX_ROWS 8

typedef struct
{
    char path[160];
    char name[64];
    char src[48]; // 来源
    long size;
    int sel;
} log_entry_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

static log_entry_t s_logs[MAX_LOGS];
static int s_n = 0;
static bool s_ready = false;
static bool s_busy = false;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_rows[MAX_ROWS] = {0};
static lv_obj_t *g_row_lbl[MAX_ROWS] = {0};
static lv_obj_t *g_hint = NULL;
static lv_timer_t *g_timer = NULL;

static int g_cur = 0;
static int g_left = 0, g_right = 0, g_enter = 0, g_rec = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static bool g_enter_long = false;

// 由文件名推断来源应用
static void guess_src(const char *name, char *out, size_t n)
{
    if (strstr(name, "lios"))
        snprintf(out, n, "桌面");
    else if (strstr(name, "crash") || strstr(name, "ui.log"))
        snprintf(out, n, "相机");
    else if (strstr(name, "perf"))
        snprintf(out, n, "性能");
    else if (strstr(name, "files_sh"))
        snprintf(out, n, "脚本输出");
    else
        snprintf(out, n, "其它");
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        size_t l = strlen(e->d_name);
        if (l < 4 || strcmp(e->d_name + l - 4, ".log") != 0)
            continue;
        if (s_n >= MAX_LOGS)
            break;
        char full[200];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        log_entry_t *en = &s_logs[s_n];
        snprintf(en->path, sizeof(en->path), "%s", full);
        snprintf(en->name, sizeof(en->name), "%s", e->d_name);
        guess_src(e->d_name, en->src, sizeof(en->src));
        en->size = (long)st.st_size;
        en->sel = 0;
        s_n++;
    }
    closedir(d);
}

static void *scan_worker(void *)
{
    pthread_mutex_lock(&s_mtx);
    s_n = 0;
    scan_dir("/tmp");
    scan_dir("/mnt/UDISK");
    s_ready = true;
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
    return NULL;
}

static void start_scan(void)
{
    pthread_mutex_lock(&s_mtx);
    s_n = 0;
    s_ready = false;
    s_busy = true;
    pthread_mutex_unlock(&s_mtx);
    pthread_t t;
    if (pthread_create(&t, NULL, scan_worker, NULL) == 0)
        pthread_detach(t);
    else
    {
        pthread_mutex_lock(&s_mtx);
        s_busy = false;
        pthread_mutex_unlock(&s_mtx);
    }
}

// 删除线程：unlink 后重新扫描（不在 UI 线程做 I/O）
static void *del_thread(void *arg)
{
    const char *p = (const char *)arg;
    unlink(p);
    pthread_mutex_lock(&s_mtx);
    s_ready = false;
    pthread_mutex_unlock(&s_mtx);
    start_scan();
    free((void *)p);
    return NULL;
}

static void do_delete(void)
{
    pthread_mutex_lock(&s_mtx);
    int idx = g_cur;
    bool have = (idx >= 0 && idx < s_n);
    char path[160];
    if (have)
        snprintf(path, sizeof(path), "%s", s_logs[idx].path);
    pthread_mutex_unlock(&s_mtx);
    if (!have)
        return;
    pthread_t t;
    if (pthread_create(&t, NULL, del_thread, strdup(path)) == 0)
        pthread_detach(t);
}

// 查看内容：弹卡片显示前 1000 字节
static lv_obj_t *s_view = NULL;
static lv_obj_t *s_view_lbl = NULL;

static void show_view(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[1200];
    size_t got = 0;
    if (f != NULL)
    {
        got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[got] = 0;
    if (got == 0)
        snprintf(buf, sizeof(buf), "(空文件)");

    if (s_view != NULL && lv_obj_is_valid(s_view))
        lv_obj_del(s_view);
    s_view = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_view, 312, 226);
    lv_obj_align(s_view, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(s_view, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_view, lv_color_hex(0x0B1116), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_view, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_view, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_view, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(s_view);

    lv_obj_t *title = lv_label_create(s_view);
    lv_obj_set_style_text_font(title, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(title, "日志内容（录像键关闭）");
    lv_obj_set_pos(title, 4, 2);

    lv_obj_t *box = lv_obj_create(s_view);
    lv_obj_set_size(box, 300, 196);
    lv_obj_set_pos(box, 2, 22);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x05080B), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1E2A36), LV_PART_MAIN | LV_STATE_DEFAULT);

    s_view_lbl = lv_label_create(box);
    lv_obj_set_width(s_view_lbl, 288);
    lv_label_set_long_mode(s_view_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_view_lbl, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_view_lbl, lv_color_hex(0xD8E2EA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_view_lbl, buf);
}

static void refresh_rows(void)
{
    pthread_mutex_lock(&s_mtx);
    bool ready = s_ready;
    int n = s_n;
    pthread_mutex_unlock(&s_mtx);
    if (!ready)
    {
        if (g_hint != NULL)
            lv_label_set_text(g_hint, "正在扫描日志…");
        return;
    }
    char b[MAX_ROWS][96];
    int rows = n > MAX_ROWS ? MAX_ROWS : n;
    if (n == 0)
    {
        for (int i = 0; i < MAX_ROWS; i++)
            if (g_row_lbl[i] != NULL)
                lv_label_set_text(g_row_lbl[i], i == 0 ? "（没有日志）" : "");
        if (g_hint != NULL)
            lv_label_set_text(g_hint, "没有日志  菜单键返回");
        return;
    }
    pthread_mutex_lock(&s_mtx);
    int start = g_cur - g_cur % MAX_ROWS;
    for (int i = 0; i < rows; i++)
    {
        int idx = start + i;
        if (idx >= n)
            break;
        log_entry_t *en = &s_logs[idx];
        char sz[16];
        if (en->size >= 1024)
            snprintf(sz, sizeof(sz), "%.1fK", en->size / 1024.0);
        else
            snprintf(sz, sizeof(sz), "%ldB", en->size);
        snprintf(b[i], 96, "%s[%s] %s  %s", idx == g_cur ? "> " : "  ",
                 en->src, en->name, sz);
    }
    pthread_mutex_unlock(&s_mtx);
    for (int i = 0; i < MAX_ROWS; i++)
        if (g_row_lbl[i] != NULL)
            lv_label_set_text(g_row_lbl[i], i < rows ? b[i] : "");
    char h[80];
    snprintf(h, sizeof(h), "%d 个日志   确认=查看   录像=删除   菜单=返回", n);
    if (g_hint != NULL)
        lv_label_set_text(g_hint, h);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (g_left > 0)
    {
        g_left--;
        pthread_mutex_lock(&s_mtx);
        if (s_n > 0)
            g_cur = (g_cur - 1 + s_n) % s_n;
        pthread_mutex_unlock(&s_mtx);
        refresh_rows();
    }
    if (g_right > 0)
    {
        g_right--;
        pthread_mutex_lock(&s_mtx);
        if (s_n > 0)
            g_cur = (g_cur + 1) % s_n;
        pthread_mutex_unlock(&s_mtx);
        refresh_rows();
    }
    if (g_enter > 0)
    {
        g_enter--;
        pthread_mutex_lock(&s_mtx);
        bool have = (g_cur >= 0 && g_cur < s_n);
        char path[160];
        if (have)
            snprintf(path, sizeof(path), "%s", s_logs[g_cur].path);
        pthread_mutex_unlock(&s_mtx);
        if (have)
            show_view(path);
    }
    if (g_rec > 0)
    {
        g_rec--;
        if (s_view != NULL && lv_obj_is_valid(s_view))
            lv_obj_del(s_view);
        s_view = NULL;
        s_view_lbl = NULL;
        do_delete();
        refresh_rows();
    }
}

extern "C" const lios_app_info_t lios_app_info = {
    "logclean", "日志清理", "清", 44, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    g_cur = 0;
    g_left = g_right = g_enter = g_rec = 0;

    for (int i = 0; i < MAX_ROWS; i++)
    {
        lv_obj_t *r = lv_label_create(parent);
        lv_obj_set_style_text_font(r, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(r, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(r, 6, 6 + i * 22);
        g_rows[i] = r;
        g_row_lbl[i] = r;
    }

    g_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(g_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_hint, "正在扫描日志…");
    lv_obj_align(g_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    start_scan();
    if (g_timer == NULL)
        g_timer = lv_timer_create(tick_cb, 40, NULL);
    return parent;
}

extern "C" void lios_app_destroy(void)
{
    if (g_timer != NULL)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
    if (s_view != NULL && lv_obj_is_valid(s_view))
        lv_obj_del(s_view);
    s_view = NULL;
    s_view_lbl = NULL;
    g_root = NULL;
    for (int i = 0; i < MAX_ROWS; i++)
    {
        g_rows[i] = NULL;
        g_row_lbl[i] = NULL;
    }
    g_hint = NULL;
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
