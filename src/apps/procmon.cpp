// LiOS 进程管理器（插件）
// 每行一个进程，套框显示：
//   进程名称（超 1/3 屏宽自动横向滚动）| 内存 | CPU% | 运行时间 | 权限 | 父进程 | 可执行路径
// 顶部标题行标注每列含义。滚轮逐行滚动（不是整页跳）。
// worker 线程以 root 读 /proc（UI 回调不做 I/O）。

#include "lios_app.h"
#include "my_main.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROC 256
#define ROWS 6

// 列布局（固定 x，右对齐式固定列）
#define COL_NAME_X 2
#define COL_MEM_X 88
#define COL_CPU_X 134
#define COL_TIME_X 168
#define COL_PERM_X 214
#define COL_PARENT_X 244
#define COL_EXE_X 266
#define COL_EXE_W (320 - COL_EXE_X - 2)

typedef struct
{
    int pid;
    char name[24];      // 进程名（comm，短）
    long rss_kb;
    int cpu;            // 0-100，-1=未知
    long etime_s;
    long uid;
    int ppid;
    char parent[24];    // 父进程名
    char exe[48];       // 可执行路径（截断）
    int is_kthread;     // 内核线程（无内存/无 exe）
} proc_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static proc_t s_procs[MAX_PROC];
static int s_n = 0;
static bool s_ready = false;

// CPU 差值采样（按 pid 索引，够大）
static long s_prev_ticks[4096];
static long s_prev_systotal = 0;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_rows[ROWS] = {0};      // 框容器
static lv_obj_t *g_name[ROWS] = {0};
static lv_obj_t *g_mem[ROWS] = {0};
static lv_obj_t *g_cpu[ROWS] = {0};
static lv_obj_t *g_time[ROWS] = {0};
static lv_obj_t *g_perm[ROWS] = {0};
static lv_obj_t *g_par[ROWS] = {0};
static lv_obj_t *g_exe[ROWS] = {0};
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
    size_t l = strlen(out);
    while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == ' ' || out[l - 1] == '\t'))
        out[--l] = 0;
}

static long field_l(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (p == NULL)
        return -1;
    return atol(p + strlen(key));
}

// 解析 /proc/pid/stat：从 ")" 后第 N 个字段（1 基，第 1 个是 state）
static long stat_field(int pid, int field)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    char buf[1024];
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = 0;
    fclose(f);
    const char *cp = strrchr(buf, ')');
    if (cp == NULL)
        return -1;
    cp += 2; // 跳过 ") "
    // 用空格拆分：第 1 个 token = state(3)，第 2 = ppid(4)...
    // field 相对：用户传的是"从 state 数第几个"
    long v = -1;
    const char *p = cp;
    for (int i = 1; i <= field; i++)
    {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        if (i == field)
        {
            char tmp[32];
            size_t len = p - start;
            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, start, len);
            tmp[len] = 0;
            v = atol(tmp);
        }
        if (*p == 0)
            break;
    }
    return v;
}

static long system_ticks(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return 1;
    long u, n, s, idl;
    if (fscanf(f, "cpu %ld %ld %ld %ld", &u, &n, &s, &idl) < 4)
    {
        fclose(f);
        return 1;
    }
    fclose(f);
    return u + n + s + idl;
}

static void *scan_worker(void *)
{
    for (;;)
    {
        long sys_total = system_ticks();
        long upt = 0;
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf != NULL)
        {
            fscanf(uf, "%lf", &upt);
            fclose(uf);
        }

        // 第一遍：pid -> name 映射（供父进程名查询）
        static char pidname[4096][24];
        DIR *d = opendir("/proc");
        if (d == NULL)
            break;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;
            int pid = atoi(e->d_name);
            if (pid <= 0 || pid >= 4096)
                continue;
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/comm", pid);
            read_file(path, pidname[pid], sizeof(pidname[pid]));
            if (pidname[pid][0] == 0)
                snprintf(pidname[pid], sizeof(pidname[pid]), "%d", pid);
        }

        // 第二遍：收集详情
        proc_t tmp[MAX_PROC];
        int n = 0;
        rewinddir(d);
        while ((e = readdir(d)) != NULL && n < MAX_PROC)
        {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;
            int pid = atoi(e->d_name);
            if (pid <= 0)
                continue;
            proc_t *p = &tmp[n];
            memset(p, 0, sizeof(*p));
            p->pid = pid;
            p->cpu = -1;

            char path[128];
            snprintf(path, sizeof(path), "/proc/%d/status", pid);
            char st[1024];
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
            p->uid = uid > 0 ? uid : 0;
            p->rss_kb = rss > 0 ? rss : -1; // 内核线程没有 VmRSS -> -1 显示 '-'
            p->is_kthread = (rss < 0);

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
                snprintf(p->exe, sizeof(p->exe), ""); // 内核线程无 exe

            // 运行时间 + CPU ticks
            long utime = stat_field(pid, 14);  // 从 state 数第14 = utime
            long stime = stat_field(pid, 15);  // 第15 = stime
            long starttime = stat_field(pid, 20); // 第20 = starttime(字段22)
            if (utime >= 0 && stime >= 0)
            {
                long tk = utime + stime;
                int idx = pid & 4095;
                if (s_prev_ticks[idx] > 0 && sys_total > 0 && s_prev_systotal > 0)
                {
                    long dts = sys_total - s_prev_systotal;
                    long dtk = tk - s_prev_ticks[idx];
                    if (dts > 0 && dtk >= 0)
                        p->cpu = (int)(dtk * 100 / dts);
                }
                s_prev_ticks[idx] = tk;
            }
            if (starttime > 0 && upt > 0)
            {
                p->etime_s = (long)(upt - starttime / 100.0);
                if (p->etime_s < 0)
                    p->etime_s = 0;
            }
            else
                p->etime_s = 0;

            // 父进程名
            if (p->ppid > 0 && p->ppid < 4096 && pidname[p->ppid][0])
                snprintf(p->parent, sizeof(p->parent), "%s", pidname[p->ppid]);
            else
                snprintf(p->parent, sizeof(p->parent), "-");
            n++;
        }
        closedir(d);
        s_prev_systotal = sys_total;

        pthread_mutex_lock(&s_mtx);
        memcpy(s_procs, tmp, sizeof(proc_t) * n);
        s_n = n;
        s_ready = true;
        pthread_mutex_unlock(&s_mtx);
        usleep(1500 * 1000);
    }
    return NULL;
}

static lv_obj_t *mk_lbl(lv_obj_t *parent, int x, int y, int w, lv_align_t al)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    if (w > 0)
        lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, al, LV_PART_MAIN | LV_STATE_DEFAULT);
    return l;
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
            if (g_name[i] != NULL)
                lv_label_set_text(g_name[i], "");
        return;
    }

    int start = g_cur; // 逐行滚动
    if (start > n - ROWS)
        start = n - ROWS;
    if (start < 0)
        start = 0;

    char b[ROWS][48];
    for (int i = 0; i < ROWS; i++)
    {
        int idx = start + i;
        if (idx >= n)
        {
            if (g_name[i] != NULL)
            {
                lv_label_set_text(g_name[i], "");
                lv_label_set_text(g_mem[i], "");
                lv_label_set_text(g_cpu[i], "");
                lv_label_set_text(g_time[i], "");
                lv_label_set_text(g_perm[i], "");
                lv_label_set_text(g_par[i], "");
                lv_label_set_text(g_exe[i], "");
                lv_obj_add_flag(g_rows[i], LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        lv_obj_clear_flag(g_rows[i], LV_OBJ_FLAG_HIDDEN);
        proc_t *p = &list[idx];
        lv_label_set_text(g_name[i], p->name);
        if (p->rss_kb < 0)
            lv_label_set_text(g_mem[i], "-");
        else if (p->rss_kb >= 1024)
        {
            snprintf(b[i], 48, "%.1fM", p->rss_kb / 1024.0);
            lv_label_set_text(g_mem[i], b[i]);
        }
        else
        {
            snprintf(b[i], 48, "%ldK", p->rss_kb);
            lv_label_set_text(g_mem[i], b[i]);
        }
        if (p->cpu < 0)
            lv_label_set_text(g_cpu[i], "-");
        else
        {
            snprintf(b[i], 48, "%d%%", p->cpu);
            lv_label_set_text(g_cpu[i], b[i]);
        }
        if (p->etime_s >= 3600)
            snprintf(b[i], 48, "%ldh", p->etime_s / 3600);
        else if (p->etime_s >= 60)
            snprintf(b[i], 48, "%ldm%lds", p->etime_s / 60, p->etime_s % 60);
        else
            snprintf(b[i], 48, "%lds", p->etime_s);
        lv_label_set_text(g_time[i], b[i]);
        snprintf(b[i], 48, "u%ld", p->uid);
        lv_label_set_text(g_perm[i], b[i]);
        lv_label_set_text(g_par[i], p->parent);
        lv_label_set_text(g_exe[i], p->exe[0] ? p->exe : "(内核)");
    }

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

    // 顶部标题行
    lv_obj_t *h1 = lv_label_create(parent);
    lv_obj_set_style_text_font(h1, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(h1, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(h1, "进程名称");
    lv_obj_set_pos(h1, COL_NAME_X, 2);
    lv_obj_t *h2 = lv_label_create(parent);
    lv_obj_set_style_text_font(h2, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(h2, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(h2, "内存");
    lv_obj_set_pos(h2, COL_MEM_X, 2);
    lv_obj_t *h3 = lv_label_create(parent);
    lv_obj_set_style_text_font(h3, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(h3, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(h3, "CPU");
    lv_obj_set_pos(h3, COL_CPU_X, 2);
    lv_obj_t *h4 = lv_label_create(parent);
    lv_obj_set_style_text_font(h4, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(h4, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(h4, "时间");
    lv_obj_set_pos(h4, COL_TIME_X, 2);
    lv_obj_t *h5 = lv_label_create(parent);
    lv_obj_set_style_text_font(h5, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(h5, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(h5, "权限");
    lv_obj_set_pos(h5, COL_PERM_X, 2);
    lv_obj_t *h6 = lv_label_create(parent);
    lv_obj_set_style_text_font(h6, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(h6, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(h6, "父");
    lv_obj_set_pos(h6, COL_PARENT_X, 2);
    lv_obj_t *h7 = lv_label_create(parent);
    lv_obj_set_style_text_font(h7, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(h7, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(h7, "路径");
    lv_obj_set_pos(h7, COL_EXE_X, 2);

    // 数据行：每行一个框
    for (int i = 0; i < ROWS; i++)
    {
        int y = 18 + i * 32;
        lv_obj_t *box = lv_obj_create(parent);
        lv_obj_set_size(box, 316, 28);
        lv_obj_set_pos(box, 2, y);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(box, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x141E28), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(box, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(box, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        g_rows[i] = box;

        // 进程名：超宽横向滚动（LONG_SCROLL 自动循环滚动）
        lv_obj_t *nm = lv_label_create(box);
        lv_obj_set_pos(nm, 2, 7);
        lv_obj_set_width(nm, COL_MEM_X - COL_NAME_X - 4);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_font(nm, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(nm, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
        g_name[i] = nm;

        g_mem[i] = mk_lbl(box, COL_MEM_X, 7, 44, LV_TEXT_ALIGN_LEFT);
        g_cpu[i] = mk_lbl(box, COL_CPU_X, 7, 32, LV_TEXT_ALIGN_LEFT);
        g_time[i] = mk_lbl(box, COL_TIME_X, 7, 44, LV_TEXT_ALIGN_LEFT);
        g_perm[i] = mk_lbl(box, COL_PERM_X, 7, 28, LV_TEXT_ALIGN_LEFT);
        g_par[i] = mk_lbl(box, COL_PARENT_X, 7, 20, LV_TEXT_ALIGN_LEFT);
        g_exe[i] = mk_lbl(box, COL_EXE_X, 7, COL_EXE_W, LV_TEXT_ALIGN_LEFT);
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
    {
        g_rows[i] = NULL;
        g_name[i] = NULL;
        g_mem[i] = NULL;
        g_cpu[i] = NULL;
        g_time[i] = NULL;
        g_perm[i] = NULL;
        g_par[i] = NULL;
        g_exe[i] = NULL;
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
}
