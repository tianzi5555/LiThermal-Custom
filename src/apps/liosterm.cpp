// LiOS Linux 终端（插件）
// 状态条（内存/CPU/存储）+ 输出区 + 预设命令列表（滚轮选择，确认执行）。
// 硬件无键盘：自由打字不现实，用"命令预设列表"执行（含常用命令、读取系统信息、
// 运行 /mnt/UDISK 下的 .sh 等）。系统权限执行（默认 root），可运行 su。
// 录像键清屏。

#include "lios_app.h"
#include "my_main.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUT_MAX 16000
#define OUT_ROWS 8
#define CMD_MAX 32
#define LIST_ROWS 5

static const char *kCmds[CMD_MAX] = {
    "ls -la /mnt/UDISK",
    "cat /proc/cpuinfo",
    "cat /proc/meminfo",
    "df -h /mnt/UDISK",
    "uptime && cat /proc/loadavg",
    "uname -a",
    "ps | head -30",
    "top -bn1 | head -20",
    "cat /sys/class/thermal/thermal_zone0/temp",
    "ifconfig 2>/dev/null; ip addr 2>/dev/null",
    "getprop ro.build.version.release 2>/dev/null; echo '非Android则上面为空'",
    "date",
    "echo '你好，LiOS 终端'; whoami; id",
    "su -c 'whoami'",
    "cat /etc/os-release 2>/dev/null",
    "mount | head -12",
    "echo hello > /tmp/term_test.txt && cat /tmp/term_test.txt",
    "sh /mnt/UDISK/fixusb.sh 2>/dev/null || echo 'fixusb.sh 不存在'",
    "ls -la /mnt/UDISK/apps",
    "cat /proc/version",
    "echo 内核启动参数; cat /proc/cmdline",
    "free -m",
    "cat /proc/uptime",
    "ls /tmp",
    "du -sh /mnt/UDISK/* 2>/dev/null | sort -rh | head -12",
    "cat /proc/partitions",
    "cat /proc/misc 2>/dev/null | head -12",
    "echo '可用命令多数在 /bin /sbin /usr/bin'; ls /bin | head -20",
    "cat /mnt/UDISK/apps/*.so > /dev/null 2>&1; echo '插件只读检查完成'",
    "echo '石头剪刀布：0石 1剪 2布'; echo $((RANDOM%3))",
    "hostname; echo $HOSTNAME",
    "cat /proc/sys/kernel/hostname 2>/dev/null; dmesg | grep -i usb | head -8"
};

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static char s_out[OUT_MAX];
static size_t s_out_len = 0;
static bool s_busy = false;
static int s_sel = 0; // 当前选中的命令

static lv_obj_t *g_root = NULL;
static lv_obj_t *g_stat = NULL;
static lv_obj_t *g_out[OUT_ROWS] = {0};
static lv_obj_t *g_list[LIST_ROWS] = {0};
static lv_obj_t *g_hint = NULL;
static lv_timer_t *g_timer = NULL;

static int g_left = 0, g_right = 0, g_enter = 0, g_rec = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static bool g_enter_long = false;

static void run_cmd(const char *cmd);
static void *run_cmd_thread(void *arg); // 前向声明（start_cmd 里用）
static void refresh_status(void);
static void refresh_ui(void);

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
        // 滚动丢弃开头
        size_t keep = OUT_MAX / 2;
        memmove(s_out, s_out + keep, s_out_len - keep);
        s_out_len -= keep;
        memcpy(s_out + s_out_len, s, n);
        s_out_len += n;
        s_out[s_out_len] = 0;
    }
}

static void start_cmd(void)
{
    const char *cmd = kCmds[s_sel];
    pthread_mutex_lock(&s_mtx);
    s_busy = true;
    char line[160];
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

static void *run_cmd_thread(void *arg) { run_cmd((const char *)arg); free(arg); return NULL; }

static void run_cmd(const char *cmd)
{
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
    FILE *f = fopen(outf, "r");
    pthread_mutex_lock(&s_mtx);
    if (f != NULL)
    {
        char buf[8000];
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = 0;
        append_out(buf, strlen(buf));
        fclose(f);
    }
    else
        append_out("(命令无输出)\n", 12);
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
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
            if (strncmp(line, "MemTotal:", 9) == 0) total = atol(line + 9);
            else if (strncmp(line, "MemAvailable:", 13) == 0) avail = atol(line + 13);
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
                if (dt > 0) cpu = (int)((dt - di) * 100 / dt);
            }
            p_total = tt; p_idle = idl;
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
    // 输出最后 OUT_ROWS 行
    pthread_mutex_lock(&s_mtx);
    char out[OUT_MAX];
    snprintf(out, sizeof(out), "%s", s_out);
    bool busy = s_busy;
    pthread_mutex_unlock(&s_mtx);
    const char *lines[OUT_ROWS];
    int n = 0;
    const char *p = out;
    while (*p && n < OUT_ROWS)
    {
        const char *nl = strchr(p, '\n');
        if (!nl) { lines[n++] = p; break; }
        lines[n++] = p;
        p = nl + 1;
    }
    for (int i = 0; i < OUT_ROWS; i++)
    {
        int idx = n - OUT_ROWS + i;
        if (idx < 0 || idx >= n || g_out[i] == NULL) { if (g_out[i]) lv_label_set_text(g_out[i], ""); continue; }
        const char *s = lines[idx];
        const char *e = strchr(s, '\n');
        char tmp[140];
        if (e)
        {
            size_t len = e - s;
            if (len > 130) { s += len - 130; len = 130; }
            memcpy(tmp, s, len); tmp[len] = 0;
        }
        else snprintf(tmp, sizeof(tmp), "%s", s);
        lv_label_set_text(g_out[i], tmp);
    }
    // 命令列表（LOW 显示当前页 5 条）
    int page = s_sel - s_sel % LIST_ROWS;
    for (int i = 0; i < LIST_ROWS; i++)
    {
        int idx = page + i;
        if (idx >= CMD_MAX || g_list[i] == NULL) { if (g_list[i]) lv_label_set_text(g_list[i], ""); continue; }
        char b[130];
        snprintf(b, sizeof(b), "%s%s", idx == s_sel ? "▶ " : "  ", kCmds[idx]);
        lv_label_set_text(g_list[i], b);
        if (g_list[i])
            lv_obj_set_style_text_color(g_list[i], idx == s_sel ? lv_color_hex(0x7DF0A0) : lv_color_hex(0x9AA7B4),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (g_hint)
        lv_label_set_text(g_hint, busy ? "执行中…" : "滚轮选命令   确认执行   录像清屏");
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    refresh_status();
    if (g_left > 0) { g_left--; s_sel = (s_sel + CMD_MAX - 1) % CMD_MAX; }
    if (g_right > 0) { g_right--; s_sel = (s_sel + 1) % CMD_MAX; }
    if (g_enter > 0) { g_enter--; if (!s_busy) start_cmd(); }
    if (g_rec > 0)
    {
        g_rec--;
        pthread_mutex_lock(&s_mtx);
        s_out_len = 0; s_out[0] = 0;
        pthread_mutex_unlock(&s_mtx);
    }
    refresh_ui();
}

extern "C" const lios_app_info_t lios_app_info = {
    "terminal", "Linux终端", "终", 46, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    s_sel = 0;
    s_out_len = 0;
    s_out[0] = 0;
    s_busy = false;

    g_stat = lv_label_create(parent);
    lv_obj_set_style_text_font(g_stat, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_stat, lv_color_hex(0x7DF0A0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(g_stat, 4, 2);

    for (int i = 0; i < OUT_ROWS; i++)
    {
        lv_obj_t *r = lv_label_create(parent);
        lv_obj_set_style_text_font(r, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(r, lv_color_hex(0xD8E2EA), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(r, 4, 18 + i * 16);
        g_out[i] = r;
    }

    // 命令列表
    for (int i = 0; i < LIST_ROWS; i++)
    {
        lv_obj_t *r = lv_label_create(parent);
        lv_obj_set_style_text_font(r, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(r, 4, 150 + i * 15);
        lv_label_set_text(r, "");
        g_list[i] = r;
    }

    g_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(g_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_hint, "滚轮选命令   确认执行   录像清屏");
    lv_obj_align(g_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    pthread_mutex_lock(&s_mtx);
    snprintf(s_out, sizeof(s_out), "LiOS Linux 终端（系统权限，当前用户 root）\n滚轮选择下方命令，确认键执行。\n\n");
    s_out_len = strlen(s_out);
    pthread_mutex_unlock(&s_mtx);

    if (g_timer == NULL)
        g_timer = lv_timer_create(tick_cb, 300, NULL);
    return parent;
}

extern "C" void lios_app_destroy(void)
{
    if (g_timer) { lv_timer_del(g_timer); g_timer = NULL; }
    g_root = NULL;
    g_stat = g_hint = NULL;
    for (int i = 0; i < OUT_ROWS; i++) g_out[i] = NULL;
    for (int i = 0; i < LIST_ROWS; i++) g_list[i] = NULL;
}

extern "C" void lios_app_key(int key, int event)
{
    if (key == LIOS_KEY_LEFT && event == LIOS_EV_PRESS) { if (g_left < 4) g_left++; }
    else if (key == LIOS_KEY_RIGHT && event == LIOS_EV_PRESS) { if (g_right < 4) g_right++; }
    else if (key == LIOS_KEY_REC && event == LIOS_EV_PRESS) { if (g_rec < 4) g_rec++; }
    else if (key == LIOS_KEY_ENTER)
    {
        if (event == LIOS_EV_PRESS)
        {
            g_enter_down = true; g_enter_ms = lv_tick_get(); g_enter_long = false;
        }
        else if (event == LIOS_EV_RELEASE)
        {
            if (g_enter_down)
            {
                g_enter_down = false;
                if (!g_enter_long && g_enter < 4) g_enter++;
            }
        }
    }
}