// LiOS 进程管理器（插件）
// 显示所有进程（/proc）：
//   进程名（超出屏宽 1/3 就横向滚动）| 内存 | CPU% | 运行时间 | 权限上下文 | 父进程 | 可执行路径
// worker 线程扫描 /proc（UI 回调不做 I/O）

#include "lios_app.h"
#include "my_main.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROC 128
#define ROWS 7

typedef struct
{
    int pid;
    char name[32];     // 进程名
    char full[64];     // 命令第一段
    long rss_kb;       // VmRSS
    int cpu;           // 占用 %
    long etime_s;      // 运行秒数
    char perm[24];     // 权限上下文 u:g
    int ppid;
    char exe[96];      // 可执行路径
} proc_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static proc_t s_procs[MAX_PROC];
static int s_n = 0;
static bool s_ready = false;
static long s_prev_total[MAX_PROC]; // 简单 CPU 估算（每进程两次采样太贵，用进程 jiffies 差值）
static long s_prev_ticks[MAX_PROC];
static long s_total_ticks[MAX_PROC];

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_rows[ROWS] = {0};
static lv_obj_t *g_hint = NULL;
static lv_timer_t *g_timer = NULL;

static int g_cur = 0;
static int g_left = 0, g_right = 0;

static void read_file(const char *path, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
    // 去尾换行
    size_t l = strlen(out);
    while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == ' '))
        out[--l] = 0;
}

static long field_l(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (p == NULL)
        return -1;
    return atol(p + strlen(key));
}

static long read_ticks(int pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    // 格式: pid (comm) state ... 从 ")" 后数第 14 个字段是 utime, 15 stime
    char buf[512];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = 0;
    fclose(f);
    const char *cp = strrchr(buf, ')');
    if (cp == NULL)
        return -1;
    cp += 2;
    // state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8) flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15)
    long v[14] = {0};
    int n = sscanf(cp, "%*s %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %ld %ld",
                   &v[0], &v[1]);
    if (n < 2)
        return -1;
    return v[0] + v[1];
}

static long total_system_ticks(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return 1;
    long u, n, s, idl, io, ir, si;
    if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld", &u, &n, &s, &idl, &io, &ir, &si) < 4)
    {
        fclose(f);
        return 1;
    }
    fclose(f);
    return u + n + s + idl + io + ir + si;
}

static void *scan_worker(void *)
{
    for (;;)
    {
        proc_t tmp[MAX_PROC];
        int n = 0;
        long sys_total = total_system_ticks();

        DIR *d = opendir("/proc");
        if (d != NULL)
        {
            struct dirent *e;
            while ((e = readdir(d)) != NULL && n < MAX_PROC)
            {
                if (e->d_name[0] < '0' || e->d_name[0] > '9')
                    continue;
                int pid = atoi(e->d_name);
                if (pid <= 0)
                    continue;
                char path[128];
                proc_t *p = &tmp[n];

                // 状态（名字/ppid/uid）
                snprintf(path, sizeof(path), "/proc/%d/status", pid);
                char st[512];
                read_file(path, st, sizeof(st));
                char nm[64];
                const char *np = strstr(st, "Name:");
                if (np != NULL)
                {
                    sscanf(np + 5, " %63s", nm);
                    snprintf(p->name, sizeof(p->name), "%s", nm);
                }
                else
                    snprintf(p->name, sizeof(p->name), "?");
                long ppid = field_l(st, "PPid:");
                long uid = field_l(st, "Uid:");
                long rss = field_l(st, "VmRSS:");
                p->ppid = (int)ppid;
                p->rss_kb = rss > 0 ? rss : 0;
                snprintf(p->perm, sizeof(p->perm), "u%ld", uid > 0 ? uid : 0);

                // cmdline 第一段
                snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
                char cl[256];
                read_file(path, cl, sizeof(cl));
                snprintf(p->full, sizeof(p->full), "%s", cl[0] ? cl : p->name);

                // exe 路径
                snprintf(path, sizeof(path), "/proc/%d/exe", pid);
                char ex[96];
                ssize_t r = readlink(path, ex, sizeof(ex) - 1);
                if (r > 0)
                {
                    ex[r] = 0;
                    snprintf(p->exe, sizeof(p->exe), "%s", ex);
                }
                else
                    snprintf(p->exe, sizeof(p->exe), "-");

                // 运行时间（HZ=100）
                long et = field_l(st, "real_time:");
                if (et < 0)
                {
                    // 从 /proc/uptime 和 starttime 算太麻烦，用 etime 替代
                    et = 0;
                }
                // 用 /proc/pid/stat 的 starttime 不准，简化：读 /proc/uptime 与 stat starttime
                snprintf(path, sizeof(path), "/proc/%d/stat", pid);
                FILE *ff = fopen(path, "r");
                if (ff != NULL)
                {
                    char b2[512];
                    size_t g2 = fread(b2, 1, sizeof(b2) - 1, ff);
                    b2[g2] = 0;
                    fclose(ff);
                    // starttime 是字段 22（从 ")" 后第 20 个数字）
                    const char *cp = strrchr(b2, ')');
                    if (cp != NULL)
                    {
                        long sv[20];
                        int cnt = sscanf(cp + 2, "%*s %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %ld", &sv[0]);
                        if (cnt == 1 && sv[0] > 0)
                        {
                            FILE *uf = fopen("/proc/uptime", "r");
                            double up = 0;
                            if (uf != NULL)
                            {
                                fscanf(uf, "%lf", &up);
                                fclose(uf);
                            }
                            p->etime_s = (long)(up - sv[0] / 100.0);
                            if (p->etime_s < 0)
                                p->etime_s = 0;
                        }
                        else
                            p->etime_s = 0;
                    }
                    else
                        p->etime_s = 0;
                }
                else
                    p->etime_s = 0;

                // CPU%：进程 ticks 差值 / 系统 ticks 差值
                long tk = read_ticks(pid);
                p->cpu = 0;
                if (tk >= 0 && s_prev_ticks[pid % MAX_PROC] > 0 && sys_total > 0)
                {
                    long dtk = tk - s_prev_ticks[pid % MAX_PROC];
                    long dts = sys_total - s_total_ticks[pid % MAX_PROC];
                    if (dtk > 0 && dts > 0)
                        p->cpu = (int)(dtk * 100 / dts);
                }
                if (s_prev_ticks[pid % MAX_PROC] == 0)
                {
                    s_prev_ticks[pid % MAX_PROC] = tk;
                    s_total_ticks[pid % MAX_PROC] = sys_total;
                }
                else
                {
                    s_prev_ticks[pid % MAX_PROC] = tk;
                    s_total_ticks[pid % MAX_PROC] = sys_total;
                }
                p->pid = pid;
                n++;
            }
            closedir(d);
        }

        pthread_mutex_lock(&s_mtx);
        memcpy(s_procs, tmp, sizeof(proc_t) * n);
        s_n = n;
        s_ready = true;
        pthread_mutex_unlock(&s_mtx);
        usleep(1500 * 1000);
    }
    return NULL;
}

static void refresh_rows(void)
{
    pthread_mutex_lock(&s_mtx);
    bool ready = s_ready;
    int n = s_n;
    proc_t list[MAX_PROC];
    if (ready)
        memcpy(list, s_procs, sizeof(proc_t) * (n > MAX_PROC ? MAX_PROC : n));
    pthread_mutex_unlock(&s_mtx);

    if (!ready)
    {
        if (g_hint != NULL)
            lv_label_set_text(g_hint, "正在扫描进程…");
        return;
    }
    if (n == 0)
    {
        for (int i = 0; i < ROWS; i++)
            if (g_rows[i] != NULL)
                lv_label_set_text(g_rows[i], i == 0 ? "（无进程）" : "");
        return;
    }

    int start = g_cur - g_cur % ROWS;
    char b[ROWS][150];
    for (int i = 0; i < ROWS; i++)
    {
        int idx = start + i;
        if (idx >= n)
            break;
        proc_t *p = &list[idx];
        char et[24];
        if (p->etime_s >= 3600)
            snprintf(et, sizeof(et), "%ldh%ldm", p->etime_s / 3600, (p->etime_s % 3600) / 60);
        else if (p->etime_s >= 60)
            snprintf(et, sizeof(et), "%ldm%lds", p->etime_s / 60, p->etime_s % 60);
        else
            snprintf(et, sizeof(et), "%lds", p->etime_s);
        char sz[16];
        if (p->rss_kb >= 1024)
            snprintf(sz, sizeof(sz), "%.1fM", p->rss_kb / 1024.0);
        else
            snprintf(sz, sizeof(sz), "%ldK", p->rss_kb);
        // 进程名（超 1/3 屏宽即滚动——这里用"名字+省略号"表示，滚动靠 LVGL 无；简单用首 12 字符+…
        // 布局：名字 | 内存 | CPU% | 时间 | 权限 | 父 | exe(截断)
        snprintf(b[i], 150, "%s  %s  %d%%  %s  %s  P%d  %s",
                 p->name, sz, p->cpu, et, p->perm, p->ppid, p->exe);
    }
    for (int i = 0; i < ROWS; i++)
        if (g_rows[i] != NULL)
            lv_label_set_text(g_rows[i], i < ROWS && (start + i) < n ? b[i] : "");

    char h[80];
    snprintf(h, sizeof(h), "%d 个进程   滚轮上下   菜单返回", n);
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
    // 每 1.5 秒自动刷新一次
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();
    if (now - last > 1500)
    {
        last = now;
        refresh_rows();
    }
}

extern "C" const lios_app_info_t lios_app_info = {
    "procmon", "进程管理", "程", 45, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    g_cur = 0;

    for (int i = 0; i < ROWS; i++)
    {
        lv_obj_t *r = lv_label_create(parent);
        lv_obj_set_style_text_font(r, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(r, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(r, 4, 6 + i * 22);
        g_rows[i] = r;
    }

    g_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(g_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_hint, "正在扫描进程…");
    lv_obj_align(g_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    static bool started = false;
    if (!started)
    {
        started = true;
        pthread_t th;
        if (pthread_create(&th, NULL, scan_worker, NULL) == 0)
            pthread_detach(th);
    }
    if (g_timer == NULL)
        g_timer = lv_timer_create(tick_cb, 100, NULL);
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
    for (int i = 0; i < ROWS; i++)
        g_rows[i] = NULL;
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
}
