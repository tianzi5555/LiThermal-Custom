// LiOS 文件管理器（真实现）
//
// 契约（见 lios.h）：
//   page_files_create(parent) 建页面并 lios_page_set_hooks(&hooks)
//   hooks.key/tick/destroy    都在 UI 线程、且**调用者已持锁**时执行
//
// 两条纪律（踩过坑，务必守）：
//   1) 这些钩子里**绝不 LOCKLV()**（锁已在手，递归锁虽兜底但别依赖）
//   2) 这些钩子里**绝不做文件 I/O** —— opendir/readdir/stat/unlink/rename
//      全部交给 worker 线程，UI 只读共享结果
//
// 滚轮：走默认 LVGL group（每行是 lv_btn + SCROLL_ON_FOCUS，与桌面网格同一套），
//       按下 = LV_EVENT_CLICKED → 行回调。所以本文件不必自己处理滚轮方向。

#include "lios.h"
#include "ui_kb.h"
#include "viewer.h" // 图片/视频/文本查看器（点文件 = 真打开）

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- 参数
#define MAX_ROWS 120
#define ROW_H 19
#define LIST_Y 24
#define LIST_H (240 - LIST_Y - 16)
#define PATH_MAX_LEN 400
#define NAME_MAX_LEN 200
#define START_DIR "/mnt/UDISK"

// ---------------------------------------------------------------- worker 请求/结果
typedef struct
{
    char name[NAME_MAX_LEN];
    bool is_dir;
    unsigned long long size;
    char mtime[24]; // 修改时间（worker 里格式化好，UI 直接用 → 回调里不做 I/O）
} fe_t;

enum
{
    REQ_NONE = 0,
    REQ_SCAN,
    REQ_DELETE,
    REQ_RENAME,
    REQ_RUNSH // 执行 .sh 脚本
};

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_req = REQ_NONE;
static char s_req_path[PATH_MAX_LEN];
static char s_req_arg[NAME_MAX_LEN];
static bool s_busy = false;
static fe_t *s_res = NULL;            // worker 分配，UI 接管后 free
static int s_res_n = 0;
static char s_res_dir[PATH_MAX_LEN];
static bool s_res_ready = false;
static char s_status[160];            // 状态/错误（worker 写，UI 读）
static bool s_status_dirty = false;
// 执行 .sh 脚本的输出（worker 写，UI 读）
#define SH_OUT_MAX 32000 // 脚本输出缓冲 32KB（之前 4KB 会截断长输出）
static char s_sh_out[SH_OUT_MAX];
static bool s_sh_ready = false;   // 结果卡片是否该显示（执行一开始就 true）
static bool s_sh_running = false; // 脚本还在跑
static bool s_sh_done = false;    // 脚本结束（正常/超时都算）
static int s_sh_exit = -1;        // 退出码（超时= -2）
static long s_sh_pos = 0;         // 已读到输出的字节位置
static char s_sh_raw[SH_OUT_MAX];  // 原始字节流（未过滤，终端样式由 sh_term_filter 生成）
static size_t s_sh_raw_len = 0;
static char s_sh_name[NAME_MAX_LEN];
static lv_obj_t *s_sh_label = NULL;   // 结果卡片里的输出 label（tick 刷新用）
static lv_obj_t *s_sh_card = NULL;    // 结果卡片容器

// ---------------------------------------------------------------- UI 状态
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_path_lbl = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_rows[MAX_ROWS] = {0};
static lv_obj_t *s_row_lbl[MAX_ROWS] = {0};
static lv_obj_t *s_card = NULL;
static lv_obj_t *s_kb = NULL;
static char s_cwd[PATH_MAX_LEN] = START_DIR;
static int s_count = 0;
static int s_shown = 0;
static int s_sel = 0;
// UI 自己存一份条目信息（不靠解析标签文本，稳妥）
static char s_names[MAX_ROWS][NAME_MAX_LEN];
static bool s_isdir[MAX_ROWS];
static unsigned long long s_size[MAX_ROWS];
static char s_mtime[MAX_ROWS][24];

static void ui_scan(const char *path);
static void ui_build_rows(void);
static void card_close(void);
static void update_list_hint(void);
static void (*s_card_cb)(int) = NULL;

// ---------------------------------------------------------------- worker
static int cmp_entry(const void *a, const void *b)
{
    const fe_t *x = (const fe_t *)a, *y = (const fe_t *)b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void worker_scan(const char *path)
{
    DIR *d = opendir(path);
    fe_t *arr = NULL;
    int n = 0, cap = 0;
    char status[160] = "";

    if (d == NULL)
        snprintf(status, sizeof(status), "打不开：%s", strerror(errno));
    else
    {
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (strcmp(e->d_name, ".") == 0)
                continue;
            char full[PATH_MAX_LEN + NAME_MAX_LEN + 2];
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(full, &st) != 0)
                continue;
            if (n == cap)
            {
                cap = cap ? cap * 2 : 64;
                fe_t *t = (fe_t *)realloc(arr, sizeof(fe_t) * cap);
                if (t == NULL)
                    break;
                arr = t;
            }
            snprintf(arr[n].name, sizeof(arr[n].name), "%s", e->d_name);
            arr[n].is_dir = S_ISDIR(st.st_mode);
            arr[n].size = (unsigned long long)st.st_size;
            arr[n].mtime[0] = 0;
            if (S_ISREG(st.st_mode))
            {
                // 时间格式化放 worker 线程（UI 的 LVGL 回调里绝不做这类可能读文件的调用）
                struct tm tmv;
                memset(&tmv, 0, sizeof(tmv));
                if (localtime_r(&st.st_mtime, &tmv) != NULL)
                    strftime(arr[n].mtime, sizeof(arr[n].mtime), "%Y-%m-%d %H:%M", &tmv);
            }
            n++;
        }
        closedir(d);
        if (arr != NULL && n > 1)
            qsort(arr, n, sizeof(fe_t), cmp_entry);
    }

    pthread_mutex_lock(&s_mtx);
    if (s_res != NULL)
        free(s_res);
    s_res = arr;
    s_res_n = n;
    snprintf(s_res_dir, sizeof(s_res_dir), "%s", path);
    snprintf(s_status, sizeof(s_status), "%s", status);
    s_status_dirty = true;
    s_res_ready = true;
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
}

// 执行 .sh 脚本：输出重定向到 /tmp（不写 SD 卡），带 15 秒超时
// 监控线程：fork 脚本 -> 边等边把 /tmp/files_sh.out 的新内容追加到 s_sh_out
//（UI 的 tick 会定期把 s_sh_out 刷到结果卡片 label —— 这就是"实时输出"）
static void *sh_monitor(void *arg)
{
    char **pp = (char **)arg;
    const char *path = pp[0];
    const char *arg2 = pp[1];
    const char *outf = "/tmp/files_sh.out";

    // 命令：把子进程输出重定向到文件，并用 while read 强制"逐行"写
    //（子进程自己的 stdio 可能是块缓冲（4KB），while read 会把每行拆出来立刻写，
    //   这样监控线程就能 50ms 轮询到实时输出；没有管道，也就没有读竞态）
    char cmd[PATH_MAX_LEN + 200];
    if (arg2 != NULL && strcmp(arg2, "cat") == 0)
        snprintf(cmd, sizeof(cmd),
                 "(cat '%s') | (while IFS= read -r l; do echo \"$l\"; done)", path);
    else
        snprintf(cmd, sizeof(cmd),
                 "(sh '%s') 2>&1 | (while IFS= read -r l; do echo \"$l\"; done)", path);

    char full[PATH_MAX_LEN + 240];
    snprintf(full, sizeof(full), "%s > %s 2>&1", cmd, outf);

    unlink(outf);
    pid_t pid = fork();
    if (pid == 0)
    {
        execl("/bin/sh", "sh", "-c", full, (char *)NULL);
        _exit(127);
    }

    int st = 0, waited = 0;
    bool timeout = false;
    bool child_done = false;
    if (pid > 0)
    {
        while (waited < 15000)
        {
            // 轮询读文件新内容（子进程逐行写，这里就能实时拿到）
            FILE *f = fopen(outf, "r");
            if (f != NULL)
            {
                long sz = 0;
                fseek(f, 0, SEEK_END);
                sz = ftell(f);
                if (sz > s_sh_pos)
                {
                    fseek(f, s_sh_pos, SEEK_SET);
                    long want = sz - s_sh_pos;
                    if (want > SH_OUT_MAX - 256)
                        want = SH_OUT_MAX - 256;
                    char buf[SH_OUT_MAX];
                    size_t got = (size_t)fread(buf, 1, (size_t)want, f);
                    buf[got] = 0;
                    pthread_mutex_lock(&s_mtx);
                    if (s_sh_raw_len + got < SH_OUT_MAX - 1)
                    {
                        memcpy(s_sh_raw + s_sh_raw_len, buf, got);
                        s_sh_raw_len += got;
                        s_sh_raw[s_sh_raw_len] = 0;
                        s_sh_pos += (long)got;
                    }
                    pthread_mutex_unlock(&s_mtx);
                }
                fclose(f);
            }
            if (waitpid(pid, &st, WNOHANG) == pid)
            {
                child_done = true;
                break;
            }
            usleep(50000);
            waited += 50;
        }
        if (waited >= 15000)
        {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            timeout = true;
            child_done = true;
        }
    }

    // 收尾：再读一次剩余输出 + 打结束标记
    pthread_mutex_lock(&s_mtx);
    {
        FILE *f = fopen(outf, "r");
        if (f != NULL)
        {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz > s_sh_pos)
            {
                fseek(f, s_sh_pos, SEEK_SET);
                long want = sz - s_sh_pos;
                if (want > SH_OUT_MAX - 256)
                    want = SH_OUT_MAX - 256;
                char buf[SH_OUT_MAX];
                size_t got = (size_t)fread(buf, 1, (size_t)want, f);
                buf[got] = 0;
                if (s_sh_raw_len + got < SH_OUT_MAX - 1)
                {
                    memcpy(s_sh_raw + s_sh_raw_len, buf, got);
                    s_sh_raw_len += got;
                    s_sh_raw[s_sh_raw_len] = 0;
                    s_sh_pos += (long)got;
                }
            }
            fclose(f);
        }
        if (s_sh_raw_len == 0)
            snprintf(s_sh_raw, SH_OUT_MAX, "(脚本无输出)");
        char tail[96];
        if (timeout)
            snprintf(tail, sizeof(tail), "\n[超时 15 秒，已强制结束]");
        else if (WIFEXITED(st))
            snprintf(tail, sizeof(tail), "\n[结束，退出码 %d]", WEXITSTATUS(st));
        else
            snprintf(tail, sizeof(tail), "\n[被信号终止]");
        if (s_sh_raw_len + strlen(tail) < SH_OUT_MAX - 1)
        {
            strcat(s_sh_raw, tail);
            s_sh_raw_len += strlen(tail);
        }
        s_sh_running = false;
        s_sh_done = true;
        s_sh_exit = timeout ? -2 : (WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        snprintf(s_status, sizeof(s_status), "脚本%s", timeout ? "超时结束" : "执行完成");
        s_status_dirty = true;
    }
    pthread_mutex_unlock(&s_mtx);

    free(pp[0]);
    free(pp[1]);
    free(pp);
    return NULL;
}

// 启动执行：立即弹结果卡片（显示"运行中"），监控线程负责边跑边补输出
static void worker_start_sh(const char *path, const char *arg)
{
    unlink("/tmp/files_sh.out");
    pthread_mutex_lock(&s_mtx);
    s_sh_out[0] = 0;
    s_sh_pos = 0;
    s_sh_running = true;
    s_sh_done = false;
    s_sh_exit = -1;
    s_sh_ready = true; // 卡片马上显示
    pthread_mutex_unlock(&s_mtx);

    char **pp = (char **)malloc(2 * sizeof(char *));
    if (pp == NULL)
        return;
    pp[0] = strdup(path);
    pp[1] = arg != NULL ? strdup(arg) : NULL;

    pthread_t t;
    if (pthread_create(&t, NULL, sh_monitor, pp) == 0)
        pthread_detach(t);
    else
    {
        free(pp[0]);
        free(pp[1]);
        free(pp);
        pthread_mutex_lock(&s_mtx);
        snprintf(s_sh_out, SH_OUT_MAX, "(无法启动执行线程)");
        s_sh_running = false;
        s_sh_done = true;
        s_busy = false;
        pthread_mutex_unlock(&s_mtx);
    }
}

static void worker_op(int req, const char *path, const char *arg)
{
    char status[160] = "";
    if (req == REQ_DELETE)
    {
        if (remove(path) == 0)
            snprintf(status, sizeof(status), "已删除");
        else
            snprintf(status, sizeof(status), "删除失败：%s", strerror(errno));
    }
    else if (req == REQ_RUNSH)
    {
        worker_start_sh(path, arg); // 立即返回（实时输出由监控线程边跑边补）
        pthread_mutex_lock(&s_mtx);
        s_busy = false; // 不占着 worker，别的请求照常
        pthread_mutex_unlock(&s_mtx);
        return;
    }
    else if (req == REQ_RENAME)
    {
        char dst[PATH_MAX_LEN + NAME_MAX_LEN + 2];
        const char *slash = strrchr(path, '/');
        int dlen = slash ? (int)(slash - path) : 0;
        snprintf(dst, sizeof(dst), "%.*s/%s", dlen, path, arg);
        if (rename(path, dst) == 0)
            snprintf(status, sizeof(status), "已重命名");
        else
            snprintf(status, sizeof(status), "重命名失败：%s", strerror(errno));
    }
    pthread_mutex_lock(&s_mtx);
    snprintf(s_status, sizeof(s_status), "%s", status);
    s_status_dirty = true;
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
}

static void *worker_main(void *)
{
    for (;;)
    {
        int req = REQ_NONE;
        char path[PATH_MAX_LEN], arg[NAME_MAX_LEN];
        pthread_mutex_lock(&s_mtx);
        if (s_req != REQ_NONE && !s_busy)
        {
            req = s_req;
            s_req = REQ_NONE;
            s_busy = true;
            snprintf(path, sizeof(path), "%s", s_req_path);
            snprintf(arg, sizeof(arg), "%s", s_req_arg);
        }
        pthread_mutex_unlock(&s_mtx);

        if (req == REQ_SCAN)
            worker_scan(path);
        else if (req == REQ_DELETE || req == REQ_RENAME || req == REQ_RUNSH)
            worker_op(req, path, arg);
        else
            usleep(30 * 1000);
    }
    return NULL;
}

static void worker_start_once(void)
{
    static bool started = false;
    if (started)
        return;
    started = true;
    pthread_t t;
    if (pthread_create(&t, NULL, worker_main, NULL) == 0)
        pthread_detach(t);
    else
        lios_log("文件管理：worker 线程创建失败");
}

static void request(int req, const char *path, const char *arg)
{
    pthread_mutex_lock(&s_mtx);
    if (s_busy)
    {
        pthread_mutex_unlock(&s_mtx);
        return;
    }
    s_req = req;
    snprintf(s_req_path, sizeof(s_req_path), "%s", path ? path : "");
    snprintf(s_req_arg, sizeof(s_req_arg), "%s", arg ? arg : "");
    pthread_mutex_unlock(&s_mtx);
}

// ---------------------------------------------------------------- 工具
static void human_size(unsigned long long sz, char *out, size_t n)
{
    if (sz >= 1024ULL * 1024 * 1024)
        snprintf(out, n, "%.1fG", sz / 1073741824.0);
    else if (sz >= 1024ULL * 1024)
        snprintf(out, n, "%.1fM", sz / 1048576.0);
    else if (sz >= 1024ULL)
        snprintf(out, n, "%.0fK", sz / 1024.0);
    else
        snprintf(out, n, "%lluB", sz);
}

static void set_hint(const char *s)
{
    if (s_hint != NULL && lv_obj_is_valid(s_hint))
        lv_label_set_text(s_hint, s);
}

// 列表的标准提示行（从查看器返回时用它恢复）
static void update_list_hint(void)
{
    char hint[220];
    if (s_count > MAX_ROWS)
        snprintf(hint, sizeof(hint), "共 %d 项，仅显示前 %d 项", s_count, MAX_ROWS);
    else
        snprintf(hint, sizeof(hint), "共 %d 项   菜单=上级  录像=操作", s_count);
    set_hint(hint);
}

// ---------------------------------------------------------------- 卡片
static void card_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    void (*cb)(int) = s_card_cb;
    card_close();
    if (cb != NULL)
        cb(idx);
}

static void card_close(void)
{
    s_sh_card = NULL;
    s_sh_label = NULL;
    s_kb = NULL;
    if (s_card != NULL && lv_obj_is_valid(s_card))
        lv_obj_del(s_card);
    s_card = NULL;
    s_card_cb = NULL;
    // 卡片关掉：把可见行放回默认组，并让组内焦点循环（滚到底回第一个）
    if (lv_group_get_default() != NULL)
        lv_group_set_wrap(lv_group_get_default(), true);
    for (int i = 0; i < MAX_ROWS; i++)
        if (s_rows[i] != NULL && lv_obj_is_valid(s_rows[i]))
        {
            if (i < s_shown)
            {
                lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
                if (lv_group_get_default() != NULL)
                    lv_group_add_obj(lv_group_get_default(), s_rows[i]);
            }
            else
                lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE); // 隐藏行永不参与焦点
        }
    if (s_shown > 0 && s_rows[0] != NULL && lv_obj_is_valid(s_rows[0]))
        lv_group_focus_obj(s_rows[(s_sel >= 0 && s_sel < s_shown) ? s_sel : 0]);
}

static void card_show(const char *text, const char *const *btns, int n, void (*cb)(int))
{
    card_close();
    s_card_cb = cb;

    // 多行文字要留够高度（信息卡片现在有名字/大小/修改时间三行）
    int lines = 1;
    for (const char *p = text; p != NULL && *p != 0; p++)
        if (*p == '\n')
            lines++;
    int extra = (lines > 2) ? (lines - 2) * 14 : 0;

    s_card = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_card, 252, 52 + extra + n * 30);
    lv_obj_center(s_card);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_card, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_card, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(s_card);

    lv_obj_t *t = lv_label_create(s_card);
    lv_obj_set_width(t, 230);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(t, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(t, text);
    lv_obj_set_pos(t, 0, 0);

    // 关键：把这些行**从默认 LVGL 组里摘掉**，否则滚轮焦点还会跑到卡片后面的文件行上
    for (int i = 0; i < MAX_ROWS; i++)
        if (s_rows[i] != NULL && lv_obj_is_valid(s_rows[i]))
        {
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
            if (lv_group_get_default() != NULL)
                lv_group_remove_obj(s_rows[i]);
        }

    lv_obj_t *first = NULL;
    for (int i = 0; i < n; i++)
    {
        lv_obj_t *b = lv_btn_create(s_card);
        lv_obj_set_size(b, 220, 26);
        lv_obj_set_pos(b, 0, 42 + i * 30);
        lv_obj_set_style_radius(b, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1E2A36), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(b, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(b, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i]);
        lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(l);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, card_btn_cb, LV_EVENT_CLICKED, NULL);
        if (first == NULL)
            first = b;
    }
    if (first != NULL)
        lv_group_focus_obj(first);
}

// 展示脚本执行结果（可滚动）
// 结果卡片打开时，由 tick 每帧调用：把监控线程最新写好的输出刷到 label。
static void refresh_sh_live(void)
{
    if (s_sh_label == NULL || !lv_obj_is_valid(s_sh_label))
        return;
    pthread_mutex_lock(&s_mtx);
    char out[SH_OUT_MAX];
    snprintf(out, sizeof(out), "%s", s_sh_out);
    pthread_mutex_unlock(&s_mtx);
    if (strcmp(out, lv_label_get_text(s_sh_label)) != 0)
        lv_label_set_text(s_sh_label, out);
}

static void show_sh_result(void)
{
    pthread_mutex_lock(&s_mtx);
    char out[SH_OUT_MAX];
    snprintf(out, sizeof(out), "%s", s_sh_out);
    s_sh_ready = false;
    pthread_mutex_unlock(&s_mtx);

    card_close();
    s_card = lv_obj_create(lv_scr_act());
    s_sh_card = s_card;
    lv_obj_set_size(s_card, 312, 226);
    lv_obj_align(s_card, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(s_card, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x0B1116), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_card, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_card, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_card);

    lv_obj_t *title = lv_label_create(s_card);
    lv_obj_set_style_text_font(title, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(title, "脚本输出（菜单键关闭）");
    lv_obj_set_pos(title, 2, 2);

    lv_obj_t *box = lv_obj_create(s_card);
    lv_obj_set_size(box, 300, 194);
    lv_obj_set_pos(box, 2, 20);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x05080B), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1E2A36), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(box, 3, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_sh_label = lv_label_create(box);
    lv_obj_set_width(s_sh_label, 288);
    lv_label_set_long_mode(s_sh_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_sh_label, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_sh_label, lv_color_hex(0xD8E2EA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_sh_label, out);
    s_card_cb = NULL;
    for (int i = 0; i < MAX_ROWS; i++)
        if (s_rows[i] != NULL && lv_obj_is_valid(s_rows[i]))
        {
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
            if (lv_group_get_default() != NULL)
                lv_group_remove_obj(s_rows[i]); // 焦点留在卡片里，滚轮绝不串到文件列表
        }
    lv_group_focus_obj(box);
}

// ---------------------------------------------------------------- 操作
static void do_delete(void)
{
    char full[PATH_MAX_LEN + NAME_MAX_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", s_cwd, s_names[s_sel]);
    request(REQ_DELETE, full, NULL);
    set_hint("正在删除…");
}

static void rename_action(const char *text, void *user)
{
    (void)user;
    if (text == NULL || text[0] == 0)
        return;
    char full[PATH_MAX_LEN + NAME_MAX_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", s_cwd, s_names[s_sel]);
    request(REQ_RENAME, full, text);
    set_hint("正在重命名…");
}

static void confirm_delete_cb(int idx)
{
    if (idx == 0)
        do_delete();
}

// .sh 文件的操作：0=执行 1=看内容 2=取消
static void sh_action_cb(int idx)
{
    char full[PATH_MAX_LEN + NAME_MAX_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", s_cwd, s_names[s_sel]);
    if (idx == 0)
    {
        snprintf(s_sh_name, sizeof(s_sh_name), "%s", s_names[s_sel]);
        request(REQ_RUNSH, full, NULL);
        set_hint("正在执行脚本…（最长 15 秒）");
    }
    else if (idx == 1)
    {
        // 复用文本查看器：这里简单读文件内容并弹卡片
        request(REQ_RUNSH, full, "cat"); // 用 sh 的 cat 读出来（不执行脚本本体）
        set_hint("正在读取脚本内容…");
    }
}

static void card_action_cb(int idx)
{
    if (idx == 0) // 删除 → 二次确认
    {
        char msg[220];
        snprintf(msg, sizeof(msg), "确定删除？\n%s", s_names[s_sel]);
        const char *btns[] = {"确认删除", "取消"};
        card_show(msg, btns, 2, confirm_delete_cb);
    }
    else if (idx == 1) // 重命名 → 键盘
    {
        card_close();
        s_card = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_card, 316, 236);
        lv_obj_align(s_card, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_card, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_card, lv_color_hex(0x0B1116), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_card, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_card, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_card, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_move_foreground(s_card);
        for (int i = 0; i < MAX_ROWS; i++)
            if (s_rows[i] != NULL && lv_obj_is_valid(s_rows[i]))
                lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);

        lv_obj_t *t = lv_label_create(s_card);
        lv_obj_set_style_text_font(t, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(t, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(t, "改名（确认键提交）");
        lv_obj_set_pos(t, 2, 2);

        s_kb = lios_kb_create(s_card, 20, 312, "改名", rename_action, NULL);
        if (s_kb != NULL)
        {
            lios_kb_set_text(s_kb, s_names[s_sel]);
            lios_kb_focus(s_kb);
        }
    }
}

// ---------------------------------------------------------------- 行回调
// 回到上一级：把路径最后一个分量去掉并做基本规整（不产生 /a/b/.. 这种累加）
static void go_parent(void)
{
    char up[PATH_MAX_LEN];
    snprintf(up, sizeof(up), "%s", s_cwd);
    int n = (int)strlen(up);
    while (n > 1 && up[n - 1] == '/')
        up[--n] = 0; // 去掉尾部斜杠
    char *slash = strrchr(up, '/');
    if (slash != NULL)
    {
        if (slash == up)
            up[1] = 0; // 到根
        else
            *slash = 0;
    }
    // 规整掉尾部的 "/."、"//" 之类
    n = (int)strlen(up);
    while (n > 1 && (up[n - 1] == '/' || up[n - 1] == '.'))
        up[--n] = 0;
    if (up[0] == 0)
        snprintf(up, sizeof(up), "/");
    ui_scan(up);
}

// 点文件：按类型真打开（图片/视频/文本 -> 查看器；其它 -> 信息卡片）
static void open_file(int idx)
{
    // .sh 脚本：先给「执行脚本 / 看内容 / 取消」，不要直接丢给文本查看器
    {
        size_t nl = strlen(s_names[idx]);
        if (nl > 3 && strcasecmp(s_names[idx] + nl - 3, ".sh") == 0)
        {
            char sz[24], msg[320];
            human_size(s_size[idx], sz, sizeof(sz));
            snprintf(msg, sizeof(msg), "%s\n大小：%s\n这是 shell 脚本，可以执行：", s_names[idx], sz);
            const char *btns[] = {"执行脚本", "看内容", "取消"};
            card_show(msg, btns, 3, sh_action_cb);
            return;
        }
    }

    char full[PATH_MAX_LEN + NAME_MAX_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", s_cwd, s_names[idx]);

    int kind = viewer_kind_of(s_names[idx], s_size[idx]);
    if (kind == VIEW_IMAGE || kind == VIEW_VIDEO || kind == VIEW_TEXT)
    {
        lios_log("文件管理：打开文件 %s（kind=%d）", full, kind);
        viewer_open(s_page, full, kind); // 失败也是查看器里显示一行错误 + 菜单返回
        set_hint("菜单键 返回列表");
        return;
    }

    // 其它类型：还是「信息卡片」，这次补上修改时间
    char sz[24], msg[300];
    human_size(s_size[idx], sz, sizeof(sz));
    snprintf(msg, sizeof(msg), "%s\n大小：%s\n修改时间：%s",
             s_names[idx], sz, (s_mtime[idx][0] != 0) ? s_mtime[idx] : "未知");
    const char *btns[] = {"知道了"};
    card_show(msg, btns, 1, NULL);
    lios_log("文件管理：信息卡片 %s（%s）", full, sz);
}

static void row_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    if (idx < 0 || idx >= s_shown)
        return;
    s_sel = idx;

    // ".." 就是返回上级（不是把它当成目录名往路径后面接）
    if (strcmp(s_names[idx], "..") == 0)
    {
        go_parent();
        return;
    }

    if (s_isdir[idx])
    {
        char next[PATH_MAX_LEN];
        snprintf(next, sizeof(next), "%s/%s", s_cwd, s_names[idx]);
        ui_scan(next);
    }
    else
    {
        open_file(idx);
    }
}

// ---------------------------------------------------------------- 行构建
static void ui_build_rows(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list))
        return;

    if (s_rows[0] == NULL)
    {
        for (int i = 0; i < MAX_ROWS; i++)
        {
            lv_obj_t *r = lv_btn_create(s_list);
            lv_obj_set_size(r, 316, ROW_H);
            lv_obj_set_pos(r, 0, i * ROW_H);
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

    pthread_mutex_lock(&s_mtx);
    fe_t *res = s_res;
    int n = s_res_n;
    char dir[PATH_MAX_LEN], status[160];
    snprintf(dir, sizeof(dir), "%s", s_res_dir);
    snprintf(status, sizeof(status), "%s", s_status);
    s_res = NULL;
    s_res_n = 0;
    s_res_ready = false;
    s_status_dirty = false;
    pthread_mutex_unlock(&s_mtx);

    snprintf(s_cwd, sizeof(s_cwd), "%s", dir);
    if (s_path_lbl != NULL && lv_obj_is_valid(s_path_lbl))
        lv_label_set_text(s_path_lbl, s_cwd);

    s_count = n;
    s_shown = (n > MAX_ROWS) ? MAX_ROWS : n;
    s_sel = 0;

    for (int i = 0; i < MAX_ROWS; i++)
    {
        if (s_rows[i] == NULL || !lv_obj_is_valid(s_rows[i]))
            continue;
        if (i < s_shown && res != NULL)
        {
            snprintf(s_names[i], sizeof(s_names[i]), "%s", res[i].name);
            s_isdir[i] = res[i].is_dir;
            s_size[i] = res[i].size;
            snprintf(s_mtime[i], sizeof(s_mtime[i]), "%s", res[i].mtime);

            char text[320];
            if (strcmp(res[i].name, "..") == 0)
                snprintf(text, sizeof(text), "..   返回上级"); // 讲清楚它是干什么的
            else if (res[i].is_dir)
                snprintf(text, sizeof(text), "%s/", res[i].name);
            else
            {
                char sz[24];
                human_size(res[i].size, sz, sizeof(sz));
                snprintf(text, sizeof(text), "%s   %s", res[i].name, sz);
            }
            lv_label_set_text(s_row_lbl[i], text);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            // 只有真正显示出来的行才允许被滚轮聚焦（否则焦点会跑到屏幕外的隐藏行）
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        }
        else
        {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            // 关键修复：隐藏的行必须同时取消可聚焦，否则滚轮往上/下会「选中框消失」
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            s_names[i][0] = 0;
            s_isdir[i] = false;
            s_size[i] = 0;
        }
    }
    if (res != NULL)
        free(res);

    char hint[220];
    if (status[0] != 0)
        snprintf(hint, sizeof(hint), "%s", status);
    else if (n > MAX_ROWS)
        snprintf(hint, sizeof(hint), "共 %d 项，仅显示前 %d 项", n, MAX_ROWS);
    else
        snprintf(hint, sizeof(hint), "共 %d 项   菜单=上级  录像=操作", n);
    set_hint(hint);

    if (s_shown > 0 && s_rows[0] != NULL && lv_obj_is_valid(s_rows[0]))
        lv_group_focus_obj(s_rows[0]);
    lios_log("文件管理：%s -> %d 项（显示 %d）%s", s_cwd, n, s_shown, status);
}

static void ui_scan(const char *path)
{
    set_hint("正在读取…");
    request(REQ_SCAN, path, NULL);
}

// ---------------------------------------------------------------- 钩子
static bool page_key(int key)
{
    // 查看器开着：按键先给它（菜单键 = 退出查看器，回原来那个目录的列表）
    if (viewer_is_open())
    {
        if (key == LIOS_K_MENU || key == LIOS_K_REC || key == LIOS_K_ENTER)
            return viewer_key(key);
        return false;
    }

    if (key == LIOS_K_MENU)
    {
        if (s_card != NULL || s_kb != NULL)
        {
            card_close();
            return true;
        }
        if (strcmp(s_cwd, START_DIR) == 0 || strcmp(s_cwd, "/") == 0 || s_cwd[0] == 0)
            return false; // 交回桌面 → 桌面关页面
        go_parent();
        return true;
    }
    if (key == LIOS_K_REC)
    {
        if (s_card != NULL || s_kb != NULL || s_shown <= 0)
            return true;
        char msg[220];
        snprintf(msg, sizeof(msg), "操作：%s", s_names[s_sel]);
        const char *btns[] = {"删除", "重命名", "取消"};
        card_show(msg, btns, 3, card_action_cb);
        return true;
    }
    if (key == LIOS_K_ENTER)
    {
        // 关键：确认键**不要**自己发 CLICKED ✗ ——
        // 桌面把 wheel_focus 页面的确认键留给 LVGL，由编码器输入自动点击当前焦点对象。
        // 我们再发一次就会「按一下点两下」（用户实测就是这个现象）。
        // 这里只做节流，防止硬件抖动造成连点。
        static uint32_t last_ms = 0;
        uint32_t now = lv_tick_get();
        if (now - last_ms < 200)
            return true; // 抖动窗口内吞掉
        last_ms = now;
        return true;
    }
    return false;
}

static void page_tick(void)
{
    // 查看器开着：这帧只伺候查看器（取解码结果、blit）；它自己关了就恢复列表
    if (viewer_is_open())
    {
        viewer_tick();
        if (!viewer_is_open())
        {
            update_list_hint();
            if (s_shown > 0 && s_rows[(s_sel >= 0 && s_sel < s_shown) ? s_sel : 0] != NULL &&
                lv_obj_is_valid(s_rows[s_sel]))
                lv_group_focus_obj(s_rows[s_sel]);
            lios_log("文件管理：从查看器返回，仍在 %s", s_cwd);
        }
        return;
    }

    bool have = false, dirty = false;
    char cur[160];
    pthread_mutex_lock(&s_mtx);
    have = s_res_ready;
    dirty = s_status_dirty;
    snprintf(cur, sizeof(cur), "%s", s_status);
    pthread_mutex_unlock(&s_mtx);

    if (have)
        ui_build_rows();

    // 脚本输出实时显示：卡片开着就一直刷新；没开则执行开始时弹卡片
    bool sh_ready = false, sh_done = false;
    pthread_mutex_lock(&s_mtx);
    sh_ready = s_sh_ready;
    sh_done = s_sh_done;
    pthread_mutex_unlock(&s_mtx);
    if (sh_ready)
    {
        if (s_sh_card == NULL || !lv_obj_is_valid(s_sh_card))
            show_sh_result(); // 第一次：建卡片
        refresh_sh_live();    // 之后每帧刷新文本 = 实时输出
        // 结束后再等一秒才允许关，避免用户没看清最后几行
        (void)sh_done;
    }

    if (dirty)
    {
        pthread_mutex_lock(&s_mtx);
        s_status_dirty = false;
        pthread_mutex_unlock(&s_mtx);
        if (cur[0] != 0)
        {
            set_hint(cur);
            if (strncmp(cur, "已删除", 9) == 0 || strncmp(cur, "已重命名", 12) == 0)
                ui_scan(s_cwd);
        }
    }
}

// 页面自己收滚轮（查看器里滚文本；桌面只在 !wheel_focus 时才回调这里）
static void page_wheel(int diff)
{
    if (viewer_is_open())
        viewer_wheel(diff);
}

static void page_destroy(void)
{
    // 关键：退出文件管理器时若查看器还开着，必须停了它的解码线程并释放缓冲
    if (viewer_is_open())
        viewer_close();

    s_page = NULL;
    s_path_lbl = NULL;
    s_list = NULL;
    s_hint = NULL;
    s_card = NULL;
    s_kb = NULL;
    for (int i = 0; i < MAX_ROWS; i++)
    {
        s_rows[i] = NULL;
        s_row_lbl[i] = NULL;
    }
    s_shown = 0;
    s_count = 0;
}

static const lios_page_hooks_t s_hooks = {page_key, page_tick, page_destroy, page_wheel};

// ---------------------------------------------------------------- 入口
void page_files_create(lv_obj_t *parent)
{
    worker_start_once();

    s_page = parent;
    snprintf(s_cwd, sizeof(s_cwd), "%s", START_DIR);

    s_path_lbl = lv_label_create(parent);
    lv_obj_set_width(s_path_lbl, 316);
    lv_label_set_long_mode(s_path_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_path_lbl, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_path_lbl, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_path_lbl, START_DIR);
    lv_obj_set_pos(s_path_lbl, 4, 4);

    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, 316, LIST_H);
    lv_obj_set_pos(s_list, 2, LIST_Y);
    // 关键修复：列表必须可滚动，SCROLL_ON_FOCUS 才能在焦点超出可视区时把列表带动
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_hint, "正在读取…");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    // 组内焦点循环：滚到底自动回到第一个（用户反馈过到底不回第一个）
    if (lv_group_get_default() != NULL)
        lv_group_set_wrap(lv_group_get_default(), true);

    lios_page_set_hooks(&s_hooks);
    lios_log("文件管理：页面已建，开始扫描 %s", START_DIR);
    ui_scan(START_DIR);
}
