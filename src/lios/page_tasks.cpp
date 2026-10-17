// LiOS 后台 / 任务管理
//
// 关键教训（用户实测）：
//   以前每次刷新都重建整个列表并在末尾强制 lv_group_focus_obj(第0行) ✗
//   → 用户滚到某行停一会儿就被拉回第一行，后面的行几乎选不了。
//   现在拆成两个函数：
//       ui_build()   —— 只在第一次建行并设焦点
//       ui_refresh() —— 只改文字的 label，（绝不碰焦点）
//
// 纪律：钩子在 UI 线程且已持锁 → 绝不 LOCKLV、绝不做文件 I/O（/proc 也在 worker 读）。

#include "lios.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ROWS 10

typedef struct
{
    long rss_kb;
    long mem_total_kb;
    long mem_avail_kb;
    float load1;
    int temp_x10;
    int child_running;
    int plugins;
    int valid;
} tasks_info_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static tasks_info_t s_info;
static bool s_ready = false;
static bool s_busy = false;
static bool s_scanf = false;
static char s_status[128];

static lv_obj_t *s_title = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_rows[MAX_ROWS] = {0};
static lv_obj_t *s_row_lbl[MAX_ROWS] = {0};
static int s_row_n = 0;
static bool s_built = false;
static char s_hint_override[128];
static uint32_t s_override_until = 0;

static void ui_build(void);
static void ui_refresh(void);
static void row_click_cb(lv_event_t *e);

static void set_hint(const char *s)
{
    if (s_hint != NULL && lv_obj_is_valid(s_hint))
        lv_label_set_text(s_hint, s);
}

// ---------------- worker
static long read_kb_field(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[256];
    long v = -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, key, strlen(key)) == 0)
        {
            v = atol(line + strlen(key));
            break;
        }
    fclose(f);
    return v;
}

static long read_self_rss(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL)
        return -1;
    char line[256];
    long v = -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, "VmRSS:", 6) == 0)
        {
            v = atol(line + 6);
            break;
        }
    fclose(f);
    return v;
}

static void worker_read(void)
{
    tasks_info_t t;
    memset(&t, 0, sizeof(t));
    t.rss_kb = read_self_rss();
    t.mem_total_kb = read_kb_field("/proc/meminfo", "MemTotal:");
    t.mem_avail_kb = read_kb_field("/proc/meminfo", "MemAvailable:");
    t.load1 = 0.0f;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f != NULL)
    {
        fscanf(f, "%f", &t.load1);
        fclose(f);
    }
    t.temp_x10 = -1000;
    f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f != NULL)
    {
        long mc = -1;
        if (fscanf(f, "%ld", &mc) == 1)
            t.temp_x10 = (int)(mc / 100);
        fclose(f);
    }
    t.child_running = lios_child_running() ? 1 : 0;
    t.plugins = lios_apps_total() - LIOS_APP_COUNT;
    if (t.plugins < 0)
        t.plugins = 0;
    t.valid = 1;

    pthread_mutex_lock(&s_mtx);
    s_info = t;
    s_ready = true;
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
}

static void *drop_thread(void *)
{
    pthread_mutex_lock(&s_mtx);
    s_busy = true;
    pthread_mutex_unlock(&s_mtx);

    sync();
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    bool ok = false;
    if (f != NULL)
    {
        ok = (fputs("1", f) >= 0);
        fclose(f);
    }
    pthread_mutex_lock(&s_mtx);
    snprintf(s_status, sizeof(s_status), ok ? "缓存已释放" : "释放失败（可能没权限）");
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
    return NULL;
}

static void *tasks_worker(void *)
{
    for (;;)
    {
        int job = 0;
        pthread_mutex_lock(&s_mtx);
        if (!s_busy)
        {
            if (s_scanf)
            {
                s_scanf = false;
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
        {
            lios_apps_scan_start();
            pthread_mutex_lock(&s_mtx);
            snprintf(s_status, sizeof(s_status), "已请求重新扫描插件目录");
            s_busy = false;
            pthread_mutex_unlock(&s_mtx);
        }
        else if (job == 2)
        {
            worker_read();
            usleep(1500 * 1000);
        }
        else
            usleep(50 * 1000);
    }
    return NULL;
}

static void tasks_worker_start(void)
{
    static bool started = false;
    if (started)
        return;
    started = true;
    pthread_t t;
    if (pthread_create(&t, NULL, tasks_worker, NULL) == 0)
        pthread_detach(t);
    else
        lios_log("后台：worker 创建失败");
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

// 只更新文字：不动焦点、不动可见性（行数固定 8 行，恒定）
static void ui_refresh(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list) || s_rows[0] == NULL)
        return;

    tasks_info_t t;
    pthread_mutex_lock(&s_mtx);
    t = s_info;
    pthread_mutex_unlock(&s_mtx);

    char b[MAX_ROWS][150];
    int n = 0;
    snprintf(b[n++], 150, "桌面占用内存   %ldK", t.rss_kb);
    snprintf(b[n++], 150, "系统内存   可用 %ldM / 共 %ldM", t.mem_avail_kb / 1024, t.mem_total_kb / 1024);
    snprintf(b[n++], 150, "系统负载   1分钟 %.2f", (double)t.load1);
    if (t.temp_x10 <= -1000)
        snprintf(b[n++], 150, "CPU 温度   --");
    else
        snprintf(b[n++], 150, "CPU 温度   %d.%d C", t.temp_x10 / 10, t.temp_x10 % 10);
    snprintf(b[n++], 150, "相机进程   %s", t.child_running ? "在运行" : "未运行");
    snprintf(b[n++], 150, "插件应用   %d 个", t.plugins);
    snprintf(b[n++], 150, "释放系统缓存      （确认执行）");
    snprintf(b[n++], 150, "重新扫描插件目录  （确认执行）");

    for (int i = 0; i < n && i < MAX_ROWS; i++)
        if (s_row_lbl[i] != NULL && lv_obj_is_valid(s_row_lbl[i]))
            lv_label_set_text(s_row_lbl[i], b[i]);
    s_row_n = n;
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
        lv_group_focus_obj(s_rows[0]); // 只在建列表时设一次焦点
    set_hint("自动刷新中   菜单=返回桌面");
    s_built = true;
}

static void row_click_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (row == 6)
    {
        pthread_t t;
        if (pthread_create(&t, NULL, drop_thread, NULL) == 0)
            pthread_detach(t);
        snprintf(s_hint_override, sizeof(s_hint_override), "正在释放缓存…");
        s_override_until = lv_tick_get() + 3000;
        set_hint(s_hint_override);
    }
    else if (row == 7)
    {
        pthread_mutex_lock(&s_mtx);
        s_scanf = true;
        pthread_mutex_unlock(&s_mtx);
        snprintf(s_hint_override, sizeof(s_hint_override), "已请求重新扫描插件目录");
        s_override_until = lv_tick_get() + 3000;
        set_hint(s_hint_override);
    }
}

static bool page_key(int key)
{
    if (key == LIOS_K_MENU)
        return false; // 回桌面
    if (key == LIOS_K_ENTER)
    {
        static uint32_t last = 0;
        uint32_t now = lv_tick_get();
        if (now - last < 200)
            return true;
        last = now;
        return true; // 真正的点击由 LVGL 对焦点对象派发
    }
    return key == LIOS_K_REC;
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
        set_hint("读取中…   菜单=返回桌面");
        return;
    }
    if (!s_built)
    {
        ui_build();
        last_refresh = now;
    }
    else if (now - last_refresh > 1500)
    {
        ui_refresh(); // 只改文字，绝不碰焦点 ✓
        last_refresh = now;
    }

    if (st[0] != 0)
    {
        snprintf(s_hint_override, sizeof(s_hint_override), "%s", st);
        s_override_until = now + 3000;
        set_hint(s_hint_override);
    }
    else if (s_override_until != 0 && now > s_override_until)
    {
        s_override_until = 0;
        set_hint("自动刷新中   菜单=返回桌面");
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

void page_tasks_create(lv_obj_t *parent)
{
    tasks_worker_start();
    s_built = false;
    s_override_until = 0;

    s_title = lv_label_create(parent);
    lv_obj_set_style_text_font(s_title, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_title, "后台 / 任务管理");
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
    lios_log("后台：页面已建");
}
