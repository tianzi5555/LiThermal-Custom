// LiOS 进程管理器 v4
//   每行一个框；所有框放在一个可滚动容器里 → 滚轮 = 整个列表平滑滚动（框跟着滑，丝滑动画）
//   进程名 / 路径：单向循环滚动（LV_LABEL_LONG_SCROLL_CIRCULAR）
//   字段：进程名称 | 内存 | CPU% | 运行时间 | 权限 | 父进程 | 路径 | 顶部标题行
//   高亮条指示选中行（容器可视区中间那行）；录像键 = 强制暂停（确认弹窗：滚轮选确认/取消，确认键确定）
//   /proc 字段索引：stat 相对 state 为 12=utime 13=stime 20=starttime 22=rss

#include "lios_app.h"
#include "my_main.h"

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROC 256
#define ROW_H 30
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
    int ppid;
    char parent[24];
    char exe[48];
} proc_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static proc_t s_procs[MAX_PROC];
static int s_n = 0;
static bool s_ready = false;
static long s_prev_ticks[4096];
static long s_prev_systotal = 0;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_cont = NULL;
static lv_obj_t *g_selbar = NULL;
static lv_obj_t *g_box[MAX_PROC];
static lv_obj_t *g_name[MAX_PROC];
static lv_obj_t *g_mem[MAX_PROC];
static lv_obj_t *g_cpu[MAX_PROC];
static lv_obj_t *g_time[MAX_PROC];
static lv_obj_t *g_perm[MAX_PROC];
static lv_obj_t *g_par[MAX_PROC];
static lv_obj_t *g_exe[MAX_PROC];
static int g_built = 0;
static lv_obj_t *g_hint = NULL;
static lv_timer_t *g_timer = NULL;

static int g_sel = 0; // 选中项索引（滚轮控制它，不是滚画面）

// 暂停确认弹窗
static lv_obj_t *g_confirm = NULL;
static lv_obj_t *g_opt[2] = {0};
static int g_confirm_sel = 0;
static int g_confirm_pid = 0;

static int g_left = 0, g_right = 0, g_enter = 0, g_rec = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static bool g_enter_long = false;

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

// stat 相对字段：1=state 2=ppid ...
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
            if (rss > 0)
                p->rss_kb = rss;
            else
            {
                long pages = stat_rel(pid, 22);
                p->rss_kb = (pages > 0) ? pages * 4 : 0;
            }
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
    if (!ready || n == 0)
        return;
    if (g_built < n)
    {
        for (int i = g_built; i < n; i++)
            build_row(i);
        g_built = n;
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
    snprintf(h, sizeof(h), "%d 个进程   滚轮滑动   录像=暂停进程", n);
    if (g_hint != NULL)
        lv_label_set_text(g_hint, h);
}

// 选中行 pid（容器可视区中间那行）
static int selected_pid(void)
{
    pthread_mutex_lock(&s_mtx);
    int n = s_n;
    int idx = g_sel;
    if (idx >= n)
        idx = n - 1;
    if (idx < 0)
        idx = 0;
    int pid = (idx >= 0 && idx < n) ? s_procs[idx].pid : 0;
    pthread_mutex_unlock(&s_mtx);
    return pid;
}

static void update_selbar(void)
{
    if (g_selbar == NULL || !lv_obj_is_valid(g_selbar))
        return;
    // 高亮条在内容坐标里跟着选中索引走（滚动时它跟着动）
    lv_obj_set_pos(g_selbar, 0, g_sel * ROW_H + 1);
}

// 滚轮移动【选中项】（不是滚画面）：
//   选中项在屏幕内逐行走；走到可见区边缘时画面才跟着滚（scroll_to_view）；
//   到第一个再上滚 -> 跳最后一个；到最后一个再下滚 -> 跳第一个（循环）
static void move_sel(int dir)
{
    pthread_mutex_lock(&s_mtx);
    int n = s_n;
    pthread_mutex_unlock(&s_mtx);
    if (n <= 0)
        return;
    g_sel = (g_sel + dir + n) % n;
    update_selbar();
    if (g_cont != NULL && lv_obj_is_valid(g_cont) && g_box[g_sel] != NULL &&
        lv_obj_is_valid(g_box[g_sel]))
        lv_obj_scroll_to_view(g_box[g_sel], LV_ANIM_ON); // 确保选中行可见（已在视野内则不动）
}

// ---------------- 暂停确认弹窗
static void close_confirm(void)
{
    if (g_confirm != NULL && lv_obj_is_valid(g_confirm))
        lv_obj_del(g_confirm);
    g_confirm = NULL;
    g_opt[0] = g_opt[1] = NULL;
}

static void update_confirm_ui(void)
{
    for (int i = 0; i < 2; i++)
    {
        if (g_opt[i] == NULL || !lv_obj_is_valid(g_opt[i]))
            continue;
        lv_obj_set_style_bg_color(g_opt[i],
                                  i == g_confirm_sel ? lv_color_hex(0x2A4A68) : lv_color_hex(0x18222C),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void open_confirm(void)
{
    int pid = selected_pid();
    if (pid <= 0)
        return;
    pthread_mutex_lock(&s_mtx);
    char name[24];
    int found = 0;
    for (int i = 0; i < s_n; i++)
        if (s_procs[i].pid == pid)
        {
            snprintf(name, sizeof(name), "%s", s_procs[i].name);
            found = 1;
            break;
        }
    pthread_mutex_unlock(&s_mtx);

    close_confirm();
    g_confirm_pid = pid;
    g_confirm_sel = 0;
    g_confirm = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_confirm, 300, 92);
    lv_obj_align(g_confirm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(g_confirm, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_confirm, lv_color_hex(0x0B1116), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_confirm, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_confirm, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(g_confirm, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(g_confirm);

    char msg[80];
    if (found)
        snprintf(msg, sizeof(msg), "暂停进程 %s (PID %d)？", name, pid);
    else
        snprintf(msg, sizeof(msg), "暂停进程 PID %d？", pid);
    lv_obj_t *t = lv_label_create(g_confirm);
    lv_obj_set_style_text_font(t, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(t, msg);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    static const char *labels[2] = {"确认暂停", "取消"};
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *b = lv_btn_create(g_confirm);
        lv_obj_set_size(b, 110, 28);
        lv_obj_set_pos(b, 12 + i * 128, 54);
        lv_obj_set_style_radius(b, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(b, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
        g_opt[i] = b;
    }
    update_confirm_ui();
}

static void do_pause(void)
{
    if (g_confirm_pid > 0)
        kill(g_confirm_pid, SIGSTOP);
    close_confirm();
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
    if (g_left > 0)
    {
        g_left--;
        if (g_confirm != NULL && lv_obj_is_valid(g_confirm))
        {
            g_confirm_sel = (g_confirm_sel + 1) % 2;
            update_confirm_ui();
        }
        else
            move_sel(-1);
    }
    if (g_right > 0)
    {
        g_right--;
        if (g_confirm != NULL && lv_obj_is_valid(g_confirm))
        {
            g_confirm_sel = (g_confirm_sel + 1) % 2;
            update_confirm_ui();
        }
        else
            move_sel(1);
    }
    if (g_rec > 0)
    {
        g_rec--;
        if (g_confirm != NULL && lv_obj_is_valid(g_confirm))
            do_pause();
        else
            open_confirm();
    }
    if (g_enter > 0)
    {
        g_enter--;
        if (g_confirm != NULL && lv_obj_is_valid(g_confirm))
        {
            if (g_confirm_sel == 0)
                do_pause();
            else
                close_confirm();
        }
    }
    update_selbar();
}

extern "C" const lios_app_info_t lios_app_info = {
    "procmon", "进程管理", "程", 45, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    g_built = 0;
    g_sel = 0;
    for (int i = 0; i < MAX_PROC; i++)
    {
        g_box[i] = NULL;
        g_name[i] = g_mem[i] = g_cpu[i] = g_time[i] = g_perm[i] = g_par[i] = g_exe[i] = NULL;
    }
    g_confirm = NULL;

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

    g_cont = lv_obj_create(parent);
    lv_obj_set_size(g_cont, 318, CONT_H);
    lv_obj_set_pos(g_cont, 1, 18);
    lv_obj_add_flag(g_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(g_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 高亮条（覆盖在容器中间行上，随容器一起滚动 -> 指示选中）
    g_selbar = lv_obj_create(g_cont);
    lv_obj_set_size(g_selbar, 314, ROW_H - 4);
    lv_obj_set_pos(g_selbar, 0, g_sel * ROW_H + 1);
    lv_obj_clear_flag(g_selbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(g_selbar, LV_OPA_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_selbar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(g_selbar, lv_color_hex(0x7DF0A0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(g_selbar, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(g_selbar);

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
    close_confirm();
    if (g_timer != NULL)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
    g_root = NULL;
    g_cont = NULL;
    g_selbar = NULL;
    g_hint = NULL;
    g_built = 0;
    g_sel = 0;
    for (int i = 0; i < MAX_PROC; i++)
    {
        g_box[i] = NULL;
        g_name[i] = g_mem[i] = g_cpu[i] = g_time[i] = g_perm[i] = g_par[i] = g_exe[i] = NULL;
    }
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