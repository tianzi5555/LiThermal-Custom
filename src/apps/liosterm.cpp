// LiOS Linux 终端（插件）—— 屏上键盘自由输入命令
// 布局：
//   第1行：内存/CPU/存储剩余
//   第2行：LiOS Linux 终端（系统权限，当前用户 root）
//   中间：输出（最后 4 行）
//   底部：屏幕键盘（自由输入命令，按「执行」运行）
// 键盘需要 LVGL 滚轮焦点 → 进页面时 lios_desktop_set_wheel_focus(true)；
// 桌面插件按键转发仍会发 LEFT/RIGHT（本应用忽略），不冲突。

#include "lios_app.h"
#include "my_main.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

// ui_kb（lios 导出，C 链接）
extern "C"
{
    typedef void (*lios_kb_action_cb)(const char *text, void *user);
    lv_obj_t *lios_kb_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t w,
                             const char *action_label, lios_kb_action_cb cb, void *user);
    const char *lios_kb_text(lv_obj_t *kb);
    void lios_kb_set_text(lv_obj_t *kb, const char *s);
    void lios_kb_clear(lv_obj_t *kb);
    void lios_kb_focus(lv_obj_t *kb);
    void lios_desktop_set_wheel_focus(bool on);
}

#define OUT_MAX 12000
#define OUT_ROWS 4
#define CMD_MAX 160

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static char s_out[OUT_MAX];
static size_t s_out_len = 0;
static bool s_busy = false;

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_stat = NULL;
static lv_obj_t *g_out[OUT_ROWS] = {0};
static lv_obj_t *g_kb = NULL;
static lv_timer_t *g_timer = NULL;

static char s_pending[CMD_MAX];
static bool s_has_pending = false;
static int g_rec = 0;

static void append_out(const char *s, size_t n)
{
    if (s_out_len + n + 1 < OUT_MAX)
    {
        memcpy(s_out + s_out_len, s, n);
        s_out_len += n;
        s_out[s_out_len] = 0;
    }
    else
    {
        size_t keep = OUT_MAX / 2;
        memmove(s_out, s_out + keep, s_out_len - keep);
        s_out_len -= keep;
        memcpy(s_out + s_out_len, s, n);
        s_out_len += n;
        s_out[s_out_len] = 0;
    }
}

static void *run_cmd_thread(void *arg);

static void start_cmd(const char *cmd)
{
    pthread_mutex_lock(&s_mtx);
    s_busy = true;
    char line[220];
    snprintf(line, sizeof(line), "\nroot@LiThermal:~$ %s\n", cmd);
    append_out(line, strlen(line));
    pthread_mutex_unlock(&s_mtx);

    pthread_t t;
    if (pthread_create(&t, NULL, run_cmd_thread, strdup(cmd)) == 0)
        pthread_detach(t);
    else
    {
        pthread_mutex_lock(&s_mtx);
        s_busy = false;
        pthread_mutex_unlock(&s_mtx);
    }
}

static void *run_cmd_thread(void *arg)
{
    char *cmd = (char *)arg;
    const char *outf = "/tmp/lios_term.out";
    unlink(outf);
    char full[512];
    snprintf(full, sizeof(full), "(%s) > %s 2>&1; echo; echo '[exit=$?]'", cmd, outf);
    pid_t pid = fork();
    if (pid == 0)
    {
        execl("/bin/sh", "sh", "-c", full, (char *)NULL);
        _exit(127);
    }
    int st = 0, waited = 0;
    if (pid > 0)
    {
        while (waited < 15000)
        {
            if (waitpid(pid, &st, WNOHANG) == pid)
                break;
            usleep(100000);
            waited += 100;
        }
        if (waited >= 15000)
        {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
    }
    pthread_mutex_lock(&s_mtx);
    FILE *f = fopen(outf, "r");
    if (f != NULL)
    {
        char buf[6000];
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = 0;
        append_out(buf, strlen(buf));
        fclose(f);
    }
    else
        append_out("(命令无输出)\n", 12);
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
    free(cmd);
    return NULL;
}

// 键盘「执行」回调（纯内存）
static void kb_action(const char *text, void *user)
{
    (void)user;
    if (text == NULL || text[0] == 0)
        return;
    snprintf(s_pending, sizeof(s_pending), "%s", text);
    s_has_pending = true;
}

static void refresh_status(void)
{
    long total = -1, avail = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    if (f)
    {
        while (fgets(line, sizeof(line), f))
        {
            if (strncmp(line, "MemTotal:", 9) == 0)
                total = atol(line + 9);
            else if (strncmp(line, "MemAvailable:", 13) == 0)
                avail = atol(line + 13);
        }
        fclose(f);
    }
    struct statvfs sv;
    double stor = -1;
    if (statvfs("/mnt/UDISK", &sv) == 0 && sv.f_blocks > 0)
        stor = (double)sv.f_bsize * sv.f_bavail / 1073741824.0;
    static long p_total = 0, p_idle = 0;
    int cpu = -1;
    f = fopen("/proc/stat", "r");
    if (f)
    {
        long u, n, s, idl;
        if (fscanf(f, "cpu %ld %ld %ld %ld", &u, &n, &s, &idl) == 4)
        {
            long tt = u + n + s + idl;
            if (p_total > 0)
            {
                long dt = tt - p_total, di = idl - p_idle;
                if (dt > 0)
                    cpu = (int)((dt - di) * 100 / dt);
            }
            p_total = tt;
            p_idle = idl;
        }
        fclose(f);
    }
    char b[128];
    if (total > 0 && avail >= 0 && stor >= 0)
        snprintf(b, sizeof(b), "内存 %ldM/%ldM  CPU %d%%  存储剩 %.1fG",
                 (total - avail) / 1024, total / 1024, cpu >= 0 ? cpu : 0, stor);
    else
        snprintf(b, sizeof(b), "内存 --  CPU --  存储 --");
    lv_label_set_text(g_stat, b);
}

static void refresh_ui(void)
{
    pthread_mutex_lock(&s_mtx);
    char out[OUT_MAX];
    snprintf(out, sizeof(out), "%s", s_out);
    pthread_mutex_unlock(&s_mtx);

    const char *lines[OUT_ROWS];
    int n = 0;
    const char *p = out;
    while (*p && n < OUT_ROWS)
    {
        const char *nl = strchr(p, '\n');
        if (!nl)
        {
            lines[n++] = p;
            break;
        }
        lines[n++] = p;
        p = nl + 1;
    }
    for (int i = 0; i < OUT_ROWS; i++)
    {
        int idx = n - OUT_ROWS + i;
        if (idx < 0 || idx >= n || g_out[i] == NULL)
        {
            if (g_out[i])
                lv_label_set_text(g_out[i], "");
            continue;
        }
        const char *s = lines[idx];
        const char *e = strchr(s, '\n');
        char tmp[140];
        if (e)
        {
            size_t len = e - s;
            if (len > 130)
            {
                s += len - 130;
                len = 130;
            }
            memcpy(tmp, s, len);
            tmp[len] = 0;
        }
        else
            snprintf(tmp, sizeof(tmp), "%s", s);
        lv_label_set_text(g_out[i], tmp);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    refresh_status();
    refresh_ui();
    if (s_has_pending)
    {
        s_has_pending = false;
        if (!s_busy)
        {
            char cmd[CMD_MAX];
            snprintf(cmd, sizeof(cmd), "%s", s_pending);
            s_pending[0] = 0;
            start_cmd(cmd);
            if (g_kb != NULL && lv_obj_is_valid(g_kb))
                lios_kb_clear(g_kb);
        }
    }
    if (g_rec > 0)
    {
        g_rec--;
        pthread_mutex_lock(&s_mtx);
        s_out_len = 0;
        s_out[0] = 0;
        pthread_mutex_unlock(&s_mtx);
    }
}

extern "C" const lios_app_info_t lios_app_info = {
    "terminal", "Linux终端", "终", 46, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    s_out_len = 0;
    s_out[0] = 0;
    s_busy = false;
    s_has_pending = false;

    // 第 1 行：内存 / CPU / 存储
    g_stat = lv_label_create(parent);
    lv_obj_set_style_text_font(g_stat, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_stat, lv_color_hex(0x7DF0A0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(g_stat, 4, 1);
    lv_label_set_text(g_stat, "内存 --  CPU --  存储 --");

    // 第 2 行：终端说明（紧挨内存行）
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(title, "LiOS Linux 终端（系统权限，当前用户 root）");
    lv_obj_set_pos(title, 4, 16);

    // 输出区（4 行）
    for (int i = 0; i < OUT_ROWS; i++)
    {
        lv_obj_t *r = lv_label_create(parent);
        lv_obj_set_style_text_font(r, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(r, lv_color_hex(0xD8E2EA), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(r, 4, 32 + i * 15);
        g_out[i] = r;
    }

    // 底部：屏幕键盘（自由输入）
    g_kb = lios_kb_create(parent, 96, 320, "执行", kb_action, NULL);
    if (g_kb != NULL)
    {
        lios_kb_set_text(g_kb, "");
        lios_desktop_set_wheel_focus(true); // 让 LVGL 收滚轮 -> 键盘可用
        lios_kb_focus(g_kb);
    }

    pthread_mutex_lock(&s_mtx);
    snprintf(s_out, sizeof(s_out), "输入命令后按键盘上的「执行」。\n例: ls /  cat /proc/cpuinfo  df -h  su\n");
    s_out_len = strlen(s_out);
    pthread_mutex_unlock(&s_mtx);

    if (g_timer == NULL)
        g_timer = lv_timer_create(tick_cb, 300, NULL);
    return parent;
}

extern "C" void lios_app_destroy(void)
{
    if (g_timer)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
    g_root = NULL;
    g_stat = NULL;
    g_kb = NULL;
    for (int i = 0; i < OUT_ROWS; i++)
        g_out[i] = NULL;
}

extern "C" void lios_app_key(int key, int event)
{
    if (key == LIOS_KEY_REC && event == LIOS_EV_PRESS)
    {
        if (g_rec < 4)
            g_rec++;
    }
}