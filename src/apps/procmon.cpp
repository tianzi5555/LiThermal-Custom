// LiOS 进程管理器 v3
//   每行一个框；所有框放在一个可滚动容器里 → 滚轮 = 整个列表平滑滚动（框跟着滑，丝滑动画）
//   进程名 / 路径：单向循环滚动（LV_LABEL_LONG_SCROLL_CIRCULAR）
//   字段：进程名称 | 内存 | CPU% | 运行时间 | 权限 | 父进程 | 路径
//   顶部标题行标注每列
//   /proc 字段索引修正：stat 字段 14=utime 15=stime 22=starttime 24=rss（相对 state 为 12/13/20/22）

#include "lios_app.h"
#include "my_main.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROC 256
#define ROW_H 30
#define ROW_GAP 2
#define CONT_H 186

#define COL_NAME_X 2
#define COL_MEM_X 86
#define COL_CPU_X 132
#define COL_TIME_X 166
#define COL_PERM_X 212
#define COL_PARENT_X 242
#define COL_EXE_X 264
#define COL_EXE_W (316 - COL_EXE_X - 2)

typedef struct
{
    int pid;
    char name[24];
    long rss_kb;
    int cpu;
    long etime_s;
    long uid;
    char parent[24];
    char exe[48];
    int is_kthread;
} proc_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static proc_t s_procs[MAX_PROC];
static int s_n = 0;
static bool s_ready = false;
static long s_prev_ticks[4096];
static long s_prev_systotal = 0;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_cont = NULL;   // 可滚动容器
static lv_obj_t *g_box[MAX_PROC]; // 行框（按进程索引建，最多 MAX_PROC）
static lv_obj_t *g_name[MAX_PROC];
static lv_obj_t *g_mem[MAX_PROC];
static lv_obj_t *g_cpu[MAX_PROC];
static lv_obj_t *g_time[MAX_PROC];
static lv_obj_t *g_perm[MAX_PROC];
static lv_obj_t *g_par[MAX_PROC];
static lv_obj_t *g_exe[MAX_PROC];
static int g_built = 0; // 已建行数
static lv_obj_t *g_hint = NULL;
static lv_timer_t *g_timer = NULL;

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

// stat 字段（相对 state 的位置：1=state, 2=ppid ...）
// 字段号 f（1 基，1=pid 2=comm 3=state）→ 相对 = f - 3 + 1
static long stat_rel(int pid, int rel)
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
    cp += 2;
    const char *p = cp;
    for (int i = 1; i <= rel; i++)
    {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        if (i == rel)
        {
            char tmp[32];
            size_t len = p - start;
            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, start, len);
            tmp[len] = 0;
            return atol(tmp);
        }
        if (*p == 0)
            break;
    }
    return -1;
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
        double upt = 0;
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf != NULL)
        {
            fscanf(uf, "%lf", &upt);
            fclose(uf);
        }

        static char pidname[4096][24];
        DIR *d = opendir("/proc");
        if (d == NULL)
        {
            usleep(1500 * 1000);
            continue;
        }
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
                snprintf(p->name, sizeof(p->name), "%s", pidname[pid]);
            long ppid = field_l(st, "PPid:");
            long uid = field_l(st, "Uid:");
            long rss = field_l(st, "VmRSS:");
            p->ppid = (int)ppid;
            p->uid = uid > 0 ? uid : 0;

            // 内存：优先 VmRSS；没有就用 stat 的 rss 字段（字段24 -> 相对22，单位页）
            if (rss > 0)
                p->rss_kb = rss;
            else
            {
                long pages = stat_rel(pid, 22);
                p->rss_kb = (pages > 0) ? pages * 4 : 0; // 4KB/页
            }
            p->is_kthread = (rss < 0);

            // exe
            snprintf(path, sizeof(path), "/proc/%d/exe", pid);
            char ex[96];
            ssize_t r = readlink(path, ex, sizeof(ex) - 1);
            if (r > 0)
            {
                ex[r] = 0;
                snprintf(p->exe, sizeof(p->exe), "%s", ex);
            }
            else
                snprintf(p->exe, sizeof(p->exe), "内核");

            // utime(字段14->相对12) stime(15->13) starttime(22->20)
            long utime = stat_rel(pid, 12);
            long stime = stat_rel(pid, 13);
            long starttime = stat_rel(pid, 20);
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

static lv_obj_t *mk_lbl(lv_obj_t *parent, int x, int y, int w, lv_text_align_t al)
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

// 建一行框（滚动容器的子对象）
static void build_row(int i)
{
    if (g_box[i] != NULL && lv_obj_is_valid(g_box[i]))
        return;
    lv_obj_t *box = lv_obj_create(g_cont);
    lv_obj_set_size(box, 316, ROW_H - 2);
    lv_obj_set_pos(box, 2, i * ROW_H);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(box, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x141E28), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    g_box[i] = box;

    // 进程名：单向循环滚动
    lv_obj_t *nm = lv_label_create(box);
    lv_obj_set_pos(nm, 2, 7);
    lv_obj_set_width(nm, COL_MEM_X - COL_NAME_X - 4);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(nm, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(nm, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    g_name[i] = nm;

    g_mem[i] = mk_lbl(box, COL_MEM_X, 7, 42, LV_TEXT_ALIGN_LEFT);
    g_cpu[i] = mk_lbl(box, COL_CPU_X, 7, 30, LV_TEXT_ALIGN_LEFT);
    g_time[i] = mk_lbl(box, COL_TIME_X, 7, 44, LV_TEXT_ALIGN_LEFT);
    g_perm[i] = mk_lbl(box, COL_PERM_X, 7, 26, LV_TEXT_ALIGN_LEFT);
    g_par[i] = mk_lbl(box, COL_PARENT_X, 7, 20, LV_TEXT_ALIGN_LEFT);

    // 路径：单向循环滚动
    lv_obj_t *ex = lv_label_create(box);
    lv_obj_set_pos(ex, COL_EXE_X, 7);
    lv_obj_set_width(ex, COL_EXE_W);
    lv_label_set_long_mode(ex, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(ex, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ex, lv_color_hex(0xA8B4C0), LV_PART_MAIN | LV_STATE_DEFAULT);
    g_exe[i] = ex;
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
        return;

    // 按需建行框（只建一次，之后只更新文字）
    if (g_built < n)
    {
        for (int i = g_built; i < n; i++)
            build_row(i);
        g_built = n;
        lv_obj_set_height(g_cont, n * ROW_H);
    }

    char b[48];
    for (int i = 0; i < n; i++)
    {
        if (g_box[i] == NULL || !lv_obj_is_valid(g_box[i]))
            continue;
        proc_t *p = &list[i];
        lv_label_set_text(g_name[i], p->name);

        if (p->rss_kb >= 1024)
            snprintf(b, sizeof(b), "%.1fM", p->rss_kb / 1024.0);
        else
            snprintf(b, sizeof(b), "%ldK", p->rss_kb);
        lv_label_set_text(g_mem[i], b);

        if (p->cpu < 0)
            lv_label_set_text(g_cpu[i], "-");
        else
        {
            snprintf(b, sizeof(b), "%d%%", p->cpu);
            lv_label_set_text(g_cpu[i], b);
        }

        if (p->etime_s >= 3600)
            snprintf(b, sizeof(b), "%ldh%ldm", p->etime_s / 3600, (p->etime_s % 3600) / 60);
        else if (p->etime_s >= 60)
            snprintf(b, sizeof(b), "%ldm%lds", p->etime_s / 60, p->etime_s % 60);
        else
            snprintf(b, sizeof(b), "%lds", p->etime_s);
        lv_label_set_text(g_time[i], b);

        snprintf(b, sizeof(b), "u%ld", p->uid);
        lv_label_set_text(g_perm[i], b);
        lv_label_set_text(g_par[i], p->parent);
        lv_label_set_text(g_exe[i], p->exe);
    }

    char h[80];
    snprintf(h, sizeof(h), "%d 个进程   滚轮滑动   菜单返回", n);
    if (g_hint != NULL)
        lv_label_set_text(g_hint, h);
}

// 滚轮：平滑滚动整个列表容器（框跟着滑）
static void scroll_list(int dir)
{
    if (g_cont == NULL || !lv_obj_is_valid(g_cont))
        return;
    lv_obj_scroll_by_bounded(g_cont, 0, -dir * ROW_H, LV_ANIM_ON);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
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
    g_built = 0;
    for (int i = 0; i < MAX_PROC; i++)
    {
        g_box[i] = NULL;
        g_name[i] = g_mem[i] = g_cpu[i] = g_time[i] = g_perm[i] = g_par[i] = g_exe[i] = NULL;
    }

    // 顶部标题行
    static const char *heads[7] = {"进程名称", "内存", "CPU", "时间", "权限", "父", "路径"};
    static const int hx[7] = {COL_NAME_X, COL_MEM_X, COL_CPU_X, COL_TIME_X, COL_PERM_X, COL_PARENT_X, COL_EXE_X};
    for (int i = 0; i < 7; i++)
    {
        lv_obj_t *h = lv_label_create(parent);
        lv_obj_set_style_text_font(h, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(h, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(h, heads[i]);
        lv_obj_set_pos(h, hx[i], 2);
    }

    // 可滚动容器
    g_cont = lv_obj_create(parent);
    lv_obj_set_size(g_cont, 318, CONT_H);
    lv_obj_set_pos(g_cont, 1, 18);
    lv_obj_add_flag(g_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(g_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

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
        g_timer = lv_timer_create(tick_cb, 200, NULL);
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
    g_cont = NULL;
    g_hint = NULL;
    g_built = 0;
    for (int i = 0; i < MAX_PROC; i++)
    {
        g_box[i] = NULL;
        g_name[i] = g_mem[i] = g_cpu[i] = g_time[i] = g_perm[i] = g_par[i] = g_exe[i] = NULL;
    }
}

extern "C" void lios_app_key(int key, int event)
{
    if (key == LIOS_KEY_LEFT && event == LIOS_EV_PRESS)
        scroll_list(-1);
    else if (key == LIOS_KEY_RIGHT && event == LIOS_EV_PRESS)
        scroll_list(1);
}
