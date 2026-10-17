// LiOS 系统监控（插件）
// 实时显示：CPU 频率 / CPU 占用 / CPU 温度 / 内存已用·剩余 / 存储已用·剩余
// worker 线程读 /proc 与 /sys（LVGL 回调不做 I/O）

#include "lios_app.h"
#include "my_main.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#define MAX_ROWS 7

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    char rows[MAX_ROWS][96];
    bool valid;
} mon_t;

static mon_t s_mon;
static bool s_ready = false;
static unsigned long long s_prev_total = 0, s_prev_idle = 0;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_labels[MAX_ROWS] = {0};
static lv_timer_t *g_timer = NULL;

static void read_kb(const char *path, const char *key, long *out)
{
    *out = -1;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, key, strlen(key)) == 0)
        {
            *out = atol(line + strlen(key));
            break;
        }
    fclose(f);
}

static long read_one(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f != NULL)
    {
        fscanf(f, "%ld", &v);
        fclose(f);
    }
    return v;
}

static int cpu_usage(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return -1;
    unsigned long long u, n, s, idl, io, ir, si;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idl, &io, &ir, &si) < 4)
    {
        fclose(f);
        return -1;
    }
    fclose(f);
    unsigned long long total = u + n + s + idl + io + ir + si;
    if (s_prev_total == 0)
    {
        s_prev_total = total;
        s_prev_idle = idl;
        return -1;
    }
    unsigned long long dt = total - s_prev_total;
    unsigned long long di = idl - s_prev_idle;
    s_prev_total = total;
    s_prev_idle = idl;
    if (dt == 0)
        return -1;
    int p = (int)((dt - di) * 100 / dt);
    return p < 0 ? 0 : (p > 100 ? 100 : p);
}

static void *mon_worker(void *)
{
    for (;;)
    {
        mon_t m;
        memset(&m, 0, sizeof(m));

        // CPU 频率（kHz）
        long freq = read_one("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
        if (freq < 0)
            freq = read_one("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq");
        if (freq > 0)
            snprintf(m.rows[0], 96, "CPU 频率    %ld MHz", freq / 1000);
        else
            snprintf(m.rows[0], 96, "CPU 频率    --");

        // CPU 占用
        int cu = cpu_usage();
        if (cu >= 0)
            snprintf(m.rows[1], 96, "CPU 占用    %d%%", cu);
        else
            snprintf(m.rows[1], 96, "CPU 占用    --");

        // CPU 温度（毫摄氏度）
        long t = read_one("/sys/class/thermal/thermal_zone0/temp");
        if (t > 0)
            snprintf(m.rows[2], 96, "CPU 温度    %ld.%ld C", t / 1000, (t % 1000) / 100);
        else
            snprintf(m.rows[2], 96, "CPU 温度    --");

        // 内存
        long total = -1, avail = -1;
        read_kb("/proc/meminfo", "MemTotal:", &total);
        read_kb("/proc/meminfo", "MemAvailable:", &avail);
        if (total > 0 && avail >= 0)
            snprintf(m.rows[3], 96, "内存已用    %ld MB  剩余 %ld MB",
                     (total - avail) / 1024, avail / 1024);
        else
            snprintf(m.rows[3], 96, "内存        --");

        // 存储 /mnt/UDISK
        struct statvfs sv;
        if (statvfs("/mnt/UDISK", &sv) == 0 && sv.f_blocks > 0)
        {
            unsigned long long tot = (unsigned long long)sv.f_bsize * sv.f_blocks;
            unsigned long long fr = (unsigned long long)sv.f_bsize * sv.f_bavail;
            unsigned long long us = tot - fr;
            snprintf(m.rows[4], 96, "存储已用    %.1f GB  剩余 %.1f GB",
                     us / 1073741824.0, fr / 1073741824.0);
        }
        else
            snprintf(m.rows[4], 96, "存储        --");

        snprintf(m.rows[5], 96, "内核        Linux 5.4.61");
        snprintf(m.rows[6], 96, "内存总计    %ld MB", total > 0 ? total / 1024 : 0);
        m.valid = true;

        pthread_mutex_lock(&s_mtx);
        s_mon = m;
        s_ready = true;
        pthread_mutex_unlock(&s_mtx);
        usleep(1000 * 1000);
    }
    return NULL;
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    pthread_mutex_lock(&s_mtx);
    mon_t m = s_mon;
    bool ready = s_ready;
    pthread_mutex_unlock(&s_mtx);
    if (!ready)
        return;
    for (int i = 0; i < MAX_ROWS; i++)
        if (g_labels[i] != NULL && lv_obj_is_valid(g_labels[i]))
            lv_label_set_text(g_labels[i], m.rows[i]);
}

extern "C" const lios_app_info_t lios_app_info = {
    "sysmon", "系统监控", "监", 43, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    s_prev_total = 0;
    s_prev_idle = 0;

    static const char *init[7] = {"CPU 频率    --", "CPU 占用    --", "CPU 温度    --",
                                  "内存已用    --", "存储已用    --", "内核        Linux",
                                  "内存总计    --"};
    for (int i = 0; i < MAX_ROWS; i++)
    {
        lv_obj_t *l = lv_label_create(parent);
        lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(l, init[i]);
        lv_obj_set_pos(l, 8, 8 + i * 24);
        g_labels[i] = l;
    }

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_style_text_font(hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(hint, "每秒自动刷新   菜单键返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    static bool started = false;
    if (!started)
    {
        started = true;
        pthread_t th;
        if (pthread_create(&th, NULL, mon_worker, NULL) == 0)
            pthread_detach(th);
    }
    if (g_timer == NULL)
        g_timer = lv_timer_create(tick_cb, 500, NULL);
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
    for (int i = 0; i < MAX_ROWS; i++)
        g_labels[i] = NULL;
}

extern "C" void lios_app_key(int key, int event)
{
    (void)key;
    (void)event;
    // 纯显示应用，不需要按键
}
