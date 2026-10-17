// LiOS 后台 / 任务管理
//
// 显示与操作（全部只读 /proc，读数在 worker 线程做）：
//   * lios 自己占用的内存（VmRSS）
//   * 系统内存（MemTotal / MemAvailable）
//   * 系统负载（/proc/loadavg）
//   * CPU 温度（/sys/class/thermal/thermal_zone0/temp）
//   * 相机进程是否在跑（lios_child_running()）
//   * 已发现的插件应用数量
// 操作（确认键 / 录像键）：
//   * 清理系统缓存：sync + 写 /proc/sys/vm/drop_caches（root 可用）
//   * 重新扫描插件目录（扫描后会通知桌面重建网格）
//
// 纪律：钩子在 UI 线程且已持锁 → 绝不 LOCKLV、绝不做文件 I/O（读 /proc 也在 worker）。

#include "lios.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ROWS 10

typedef struct
{
    long rss_kb;        // lios 自身 RSS
    long mem_total_kb;  // 总内存
    long mem_avail_kb;  // 可用内存
    float load1;        // 1 分钟负载
    int temp_x10;       // CPU 温度 x10，-1000 = 不可用
    int child_running;  // 相机进程
    int plugins;        // 插件数量
    int valid;
} tasks_info_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static tasks_info_t s_info;
static bool s_ready = false;
static bool s_busy = false;
static bool s_scanf = false;   // 请求重新扫描插件
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
static long read_kb_field(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[256];
    long v = -1;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, key, strlen(key)) == 0)
        {
            v = atol(line + strlen(key));
            break;
        }
    }
    fclose(f);
    return v; // meminfo 是 kB
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

static void worker_drop_caches(void)
{
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
}

// 独立线程做一次性动作（不能在 LVGL 回调里做 I/O）
static void *drop_thread(void *)
{
    pthread_mutex_lock(&s_mtx);
    s_busy = true;
    pthread_mutex_unlock(&s_mtx);
    worker_drop_caches();
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
                job = 2; // 周期性刷新读数
            }
        }
        pthread_mutex_unlock(&s_mtx);

        if (job == 1)
        {
            lios_apps_scan_start(); // 已扫过则内部忽略；桌面会在 revision 变化时重建网格
            pthread_mutex_lock(&s_mtx);
            snprintf(s_status, sizeof(s_status), "已请求重新扫描插件目录");
            s_busy = false;
            pthread_mutex_unlock(&s_mtx);
        }
        else if (job == 2)
        {
            worker_read();
            usleep(1500 * 1000); // 1.5 秒刷新一次
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
static void fmt_mem(long kb, char *out, size_t n)
{
    if (kb < 0)
        snprintf(out, n, "--");
    else if (kb >= 1024)
        snprintf(out, n, "%.1fM", kb / 1024.0);
    else
        snprintf(out, n, "%ldK", kb);
}

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

    tasks_info_t t;
    pthread_mutex_lock(&s_mtx);
    t = s_info;
    pthread_mutex_unlock(&s_mtx);

    char b[8][128];
    char m1[32], m2[32];
    fmt_mem(t.rss_kb, m1, sizeof(m1));
    int n = 0;

    snprintf(b[n++], 128, "桌面占用内存   %s", m1);
    snprintf(b[n++], 128, "系统内存   可用 %ldM / 共 %ldM",
             t.mem_avail_kb / 1024, t.mem_total_kb / 1024);
    snprintf(b[n++], 128, "系统负载   1分钟 %.2f", t.load1);
    if (t.temp_x10 <= -1000)
        snprintf(b[n++], 128, "CPU 温度   --");
    else
        snprintf(b[n++], 128, "CPU 温度   %d.%d C", t.temp_x10 / 10, t.temp_x10 % 10);
    snprintf(b[n++], 128, "相机进程   %s", t.child_running ? "在运行" : "未运行");
    snprintf(b[n++], 128, "插件应用   %d 个", t.plugins);
    snprintf(b[n++], 128, "释放系统缓存      （确认执行）");
    snprintf(b[n++], 128, "重新扫描插件目录  （确认执行）");
    (void)m2;

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
    if (row == 6)
    {
        pthread_t t;
        if (pthread_create(&t, NULL, drop_thread, NULL) == 0)
            pthread_detach(t);
        if (s_hint != NULL && lv_obj_is_valid(s_hint))
            lv_label_set_text(s_hint, "正在释放缓存…");
    }
    else if (row == 7)
    {
        pthread_mutex_lock(&s_mtx);
        s_scanf = true;
        pthread_mutex_unlock(&s_mtx);
        if (s_hint != NULL && lv_obj_is_valid(s_hint))
            lv_label_set_text(s_hint, "已请求重新扫描插件目录");
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
        return true;
    }
    return key == LIOS_K_REC;
}

static void page_tick(void)
{
    static bool built = false;
    static uint32_t last_rebuild = 0;
    uint32_t now = lv_tick_get();

    pthread_mutex_lock(&s_mtx);
    bool ready = s_ready;
    char st[128];
    snprintf(st, sizeof(st), "%s", s_status);
    pthread_mutex_unlock(&s_mtx);

    if (ready && (!built || now - last_rebuild > 1500))
    {
        built = true;
        last_rebuild = now;
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

void page_tasks_create(lv_obj_t *parent)
{
    tasks_worker_start();

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
