// SH 终端 —— 在相机界面里直接执行高权限（root）shell 命令
//
// 用途（重要）：
//   * 任何时候出问题，都可以在这里自救，不需要电脑、不需要 adb
//   * 卡片的两个快捷键：
//       「USB 状态」= 查看 otg_role / UDC / gadget 配置
//       「恢复 USB」= echo usb_device > otg_role; setusbconfig mtp,adb  ← 一键把 USB 恢复成原生 adb+MTP
//
// 说明：命令在 UI 线程里执行，最多等 TERM_TIMEOUT_MS，超时就杀掉子进程（避免界面卡死）。

#include "my_main.h"
#include "uiKeyboard.h"
#include "uiTrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#define TERM_CARD_W 314
#define TERM_CARD_H 236
#define TERM_CARD_Y 2
#define TERM_TIMEOUT_MS 5000
#define TERM_OUT_MAX 3000

static MyCard card_term;
static bool term_open = false;
static ui_kb_t *kb = NULL;
static lv_obj_t *out_cont = NULL; // 输出区（可滚动）
static lv_obj_t *lbl_hint = NULL;

static const char *ROLE_PATH = "/sys/devices/platform/soc@3000000/soc@3000000:usbc0@0/otg_role";

// ---------------------------------------------------------------- 执行命令（带超时）
static void run_cmd(const char *cmd, char *out, size_t outn)
{
    out[0] = 0;
    if (!cmd || !cmd[0])
        return;

    int fds[2];
    if (pipe(fds) != 0)
        return;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0)
    {
        // 子进程：stdout+stderr 都接到管道
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    size_t got = 0;
    struct timeval start, now;
    gettimeofday(&start, NULL);
    int timed_out = 0;

    while (got + 1 < outn)
    {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fds[0], &rs);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms
        int r = select(fds[0] + 1, &rs, NULL, NULL, &tv);
        if (r > 0)
        {
            ssize_t n = read(fds[0], out + got, outn - got - 1);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        else if (r < 0)
        {
            break;
        }
        gettimeofday(&now, NULL);
        long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (ms >= TERM_TIMEOUT_MS)
        {
            timed_out = 1;
            break;
        }
    }
    out[got] = 0;

    if (timed_out)
    {
        kill(pid, SIGKILL);
        if (got + 24 < outn)
            strncat(out, "\n[命令超时，已强制结束]\n", outn - got - 1);
    }
    waitpid(pid, NULL, 0);
    close(fds[0]);
}

// ---------------------------------------------------------------- 输出区
static void out_clear(void)
{
    if (out_cont && lv_obj_is_valid(out_cont))
        lv_obj_clean(out_cont);
}

static void out_append(const char *s)
{
    if (!out_cont || !lv_obj_is_valid(out_cont) || !s)
        return;

    // 限制总长度：太长了就把最老的删掉
    while (lv_obj_get_child_cnt(out_cont) > 60)
        lv_obj_del(lv_obj_get_child(out_cont, 0));

    lv_obj_t *lab = lv_label_create(out_cont);
    lv_obj_set_width(lab, TERM_CARD_W - 20);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xD8E2EA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(lab, s);
    lv_obj_scroll_to_view(lab, LV_ANIM_OFF);
}

// ---------------------------------------------------------------- 执行 + 显示
static void do_exec(const char *cmd)
{
    if (!cmd || !cmd[0])
        return;

    char out[TERM_OUT_MAX];
    char head[600];
    snprintf(head, sizeof(head), "# %s\n", cmd);
    out_append(head);

    ui_trace("terminal: 执行 %s", cmd);
    run_cmd(cmd, out, sizeof(out));

    if (out[0])
        out_append(out);
    else
        out_append("(无输出)\n");
}

static void kb_action(const char *text, void *user)
{
    (void)user;
    if (!text || !text[0])
        return;
    do_exec(text);
    if (kb)
        ui_kb_clear(kb);
}

// ---------------------------------------------------------------- 快捷键
static void btn_usb_status_cb(lv_event_t *e)
{
    (void)e;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "echo -n 'otg_role = '; cat %s; "
             "echo -n 'UDC      = '; cat /sys/kernel/config/usb_gadget/g1/UDC; "
             "echo -n 'configs  = '; ls /sys/kernel/config/usb_gadget/g1/configs/c.1; "
             "echo -n '网卡     = '; ls /sys/class/net",
             ROLE_PATH);
    do_exec(cmd);
}

static void btn_usb_restore_cb(lv_event_t *e)
{
    (void)e;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "echo usb_device > %s; sleep 1; /bin/setusbconfig mtp,adb; "
             "echo '已执行：echo usb_device > otg_role; setusbconfig mtp,adb'; "
             "echo -n 'otg_role = '; cat %s",
             ROLE_PATH, ROLE_PATH);
    do_exec(cmd);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    menu_system_show();
    menu_terminal_hide();
}

// ---------------------------------------------------------------- 页面
void menu_terminal_show(void)
{
    if (term_open)
        return;

    card_term.create(lv_layer_top(), 0, TERM_CARD_Y, TERM_CARD_W, TERM_CARD_H, LV_ALIGN_TOP_MID);
    lv_obj_t *root = card_term.obj;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(root, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(root, 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 标题
    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_pos(title, 2, 0);
    lv_label_set_text(title, "SH 终端（root）");
    lv_obj_set_style_text_font(title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 两个保命快捷键
    lv_obj_t *b1 = lv_btn_create(root);
    lv_obj_set_size(b1, 84, 20);
    lv_obj_set_pos(b1, TERM_CARD_W - 178, 0);
    lv_obj_clear_flag(b1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l1 = lv_label_create(b1);
    lv_label_set_text(l1, "USB 状态");
    lv_obj_set_style_text_font(l1, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(l1);
    lv_obj_add_event_cb(b1, btn_usb_status_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b2 = lv_btn_create(root);
    lv_obj_set_size(b2, 84, 20);
    lv_obj_set_pos(b2, TERM_CARD_W - 90, 0);
    lv_obj_clear_flag(b2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(b2, lv_color_hex(0x1E7F4F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *l2 = lv_label_create(b2);
    lv_label_set_text(l2, "恢复 USB");
    lv_obj_set_style_text_font(l2, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(l2);
    lv_obj_add_event_cb(b2, btn_usb_restore_cb, LV_EVENT_CLICKED, NULL);

    // 提示行
    lbl_hint = lv_label_create(root);
    lv_obj_set_pos(lbl_hint, 2, 22);
    lv_label_set_text(lbl_hint, "输入命令后按「执行」；USB 出问题先按「恢复 USB」");
    lv_obj_set_style_text_font(lbl_hint, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_hint, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);

    // 输出区
    out_cont = lv_obj_create(root);
    lv_obj_set_size(out_cont, TERM_CARD_W - 8, 78);
    lv_obj_set_pos(out_cont, 0, 40);
    lv_obj_set_flex_flow(out_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(out_cont, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(out_cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(out_cont, lv_color_hex(0x0B1116), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(out_cont, LV_SCROLLBAR_MODE_AUTO);
    out_append("就绪。示例：\n  uname -a\n  df -h\n  ls /mnt/UDISK\n  sh /mnt/UDISK/fixusb.sh\n");

    // 键盘（含输入框），放最下面
    kb = ui_kb_create(root, 122, TERM_CARD_W - 8, "执行", kb_action, NULL);

    card_term.show(CARD_ANIM_FLY_UP);
    term_open = true;
    if (kb)
        ui_kb_focus(kb);
}

void menu_terminal_hide(void)
{
    if (!term_open)
        return;
    card_term.hide(CARD_ANIM_FLY_DOWN);
    term_open = false;
    kb = NULL;
    out_cont = NULL;
    lbl_hint = NULL;
}

bool menu_terminal_is_open(void)
{
    return term_open;
}

// 编码器/按键在终端页时的处理：返回键 = 回系统设置
void menu_terminal_loop(bool go_back)
{
    if (!term_open)
        return;
    if (go_back)
    {
        menu_system_show();
        menu_terminal_hide();
    }
}
