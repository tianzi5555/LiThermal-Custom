// SH 终端 —— 在相机界面里以 root 执行 shell 命令（隐藏入口：系统设置→手动清除噪声→录像键）
//
// 这一版修掉的问题：
//   * 退出后菜单键失效 / 进不去第二次 —— 退出时还原 current_mode，并且用 MyCard::del（会把 obj 置空）
//   * 只显示命令不显示输出 —— 改成「命令输出重定向到文件，再读回来显示」，不再用 pipe+select
//   * 输入框被键盘压住 —— 输入框加高、加亮边框，键盘整体下移
//
// 卡片右上三个快捷键：
//   「USB 状态」= 看 otg_role / UDC / gadget 配置
//   「恢复 USB」= echo usb_device > otg_role; setusbconfig mtp,adb   ← 一键恢复原生 adb+MTP
//   「清除日志」= 删 /mnt/UDISK/*.log 并报告释放的空间

#include "my_main.h"
#include "uiKeyboard.h"
#include "uiTrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#define TERM_CARD_W 314
#define TERM_CARD_H 236
#define TERM_CARD_Y 2
#define TERM_BTN_X 2                                    // 顶部快捷键行起点
#define TERM_BTN_PITCH 63                               // 60 宽 + 3 间距（4 个键）
#define TERM_TIMEOUT_MS 8000
#define TERM_OUT_MAX 3000
#define TERM_OUTF "/tmp/term.out"

static MyCard card_term;
static bool term_open = false;
static ui_kb_t *kb = NULL;
static lv_obj_t *out_cont = NULL;

static const char *ROLE_PATH = "/sys/devices/platform/soc@3000000/soc@3000000:usbc0@0/otg_role";

// ---------------------------------------------------------------- 执行命令（输出写文件再读回）
static void run_cmd(const char *cmd, char *out, size_t outn)
{
    out[0] = 0;
    if (!cmd || !cmd[0])
        return;

    unlink(TERM_OUTF);
    char full[1024];
    snprintf(full, sizeof(full), "(%s) > %s 2>&1", cmd, TERM_OUTF);

    pid_t pid = fork();
    if (pid == 0)
    {
        execl("/bin/sh", "sh", "-c", full, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
    {
        snprintf(out, outn, "[fork 失败]\n");
        return;
    }

    int status = 0, waited = 0;
    while (waited < TERM_TIMEOUT_MS)
    {
        if (waitpid(pid, &status, WNOHANG) == pid)
            break;
        usleep(100000);
        waited += 100;
    }
    if (waited >= TERM_TIMEOUT_MS)
    {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    FILE *f = fopen(TERM_OUTF, "r");
    if (f)
    {
        size_t got = fread(out, 1, outn - 1, f);
        out[got] = 0;
        fclose(f);
    }
    if (!out[0])
        snprintf(out, outn, "(无输出)\n");
    else if (waited >= TERM_TIMEOUT_MS && strlen(out) + 24 < outn)
        strcat(out, "\n[命令超时，已强制结束]\n");
}

// ---------------------------------------------------------------- 输出区
static void out_append(const char *s)
{
    if (!out_cont || !lv_obj_is_valid(out_cont) || !s || !s[0])
        return;
    while (lv_obj_get_child_cnt(out_cont) > 40)
        lv_obj_del(lv_obj_get_child(out_cont, 0));

    lv_obj_t *lab = lv_label_create(out_cont);
    lv_obj_set_width(lab, TERM_CARD_W - 24);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xD8E2EA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(lab, s);
    lv_obj_scroll_to_view(lab, LV_ANIM_OFF);
}

static void do_exec(const char *cmd)
{
    if (!cmd || !cmd[0])
        return;
    char head[600];
    snprintf(head, sizeof(head), "# %s", cmd);
    out_append(head);

    static char out[TERM_OUT_MAX];
    ui_trace("terminal: 执行 %s", cmd);
    run_cmd(cmd, out, sizeof(out));
    out_append(out);
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
             "echo -n 'otg_role='; cat %s; "
             "echo -n 'UDC='; cat /sys/kernel/config/usb_gadget/g1/UDC; "
             "echo -n 'configs='; ls /sys/kernel/config/usb_gadget/g1/configs/c.1; "
             "echo -n 'net='; ls /sys/class/net",
             ROLE_PATH);
    do_exec(cmd);
}

static void btn_usb_restore_cb(lv_event_t *e)
{
    (void)e;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "echo usb_device > %s; sleep 1; /bin/setusbconfig mtp,adb; "
             "echo -n 'otg_role='; cat %s",
             ROLE_PATH, ROLE_PATH);
    do_exec(cmd);
}

static void btn_clear_logs_cb(lv_event_t *e)
{
    (void)e;
    do_exec("b=$(df /mnt/UDISK | awk 'NR==2{print $4}'); "
            "ls -l /mnt/UDISK/*.log 2>/dev/null; "
            "rm -f /mnt/UDISK/*.log; sync; "
            "a=$(df /mnt/UDISK | awk 'NR==2{print $4}'); "
            "echo \"可用 ${b}KB -> ${a}KB\"");
}

// 进 LiOS：写标记文件后**退出相机进程**，由 /mnt/UDISK/thermalcamera.sh 接手启动 /mnt/UDISK/lios。
// 这样两个程序是平级的独立进程，互不影响，也不会有模式切换/锁的问题。
static void btn_lios_cb(lv_event_t *e)
{
    (void)e;
    ui_trace("terminal: 按「进LiOS」-> 写 /tmp/lios_mode=lios 并退出相机进程");
    FILE *f = fopen("/tmp/lios_mode", "w");
    if (f != NULL)
    {
        fputs("lios", f);
        fclose(f);
    }
    settings_save();          // 先把相机设置落盘（和正常退出一样）
    videoPlayer.disconnect(); // 断开 RTSP，把显示/网络让给 lios
    sync();
    _exit(0); // 退出码 0：启动脚本会看到 lios 标记并去跑 lios
}

// ---------------------------------------------------------------- 页面
void menu_terminal_show(void)
{
    if (term_open)
        return;

    ui_trace("terminal: show 开始");
    card_term.create(lv_layer_top(), 0, TERM_CARD_Y, TERM_CARD_W, TERM_CARD_H, LV_ALIGN_TOP_MID);
    lv_obj_t *root = card_term.obj;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(root, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(root, 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_pos(title, 2, 2);
    lv_label_set_text(title, "SH 终端 root");
    lv_obj_set_style_text_font(title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *b1 = lv_btn_create(root);
    lv_obj_set_size(b1, 60, 26);
    lv_obj_set_pos(b1, TERM_BTN_X, 0);
    lv_obj_clear_flag(b1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(b1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b1, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *l1 = lv_label_create(b1);
    lv_label_set_text(l1, "USB状态");
    lv_obj_set_style_text_font(l1, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(l1);
    lv_obj_add_event_cb(b1, btn_usb_status_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b2 = lv_btn_create(root);
    lv_obj_set_size(b2, 60, 26);
    lv_obj_set_pos(b2, TERM_BTN_X + TERM_BTN_PITCH, 0);
    lv_obj_clear_flag(b2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(b2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b2, lv_color_hex(0x1E7F4F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *l2 = lv_label_create(b2);
    lv_label_set_text(l2, "恢复USB");
    lv_obj_set_style_text_font(l2, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(l2);
    lv_obj_add_event_cb(b2, btn_usb_restore_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b3 = lv_btn_create(root);
    lv_obj_set_size(b3, 60, 26);
    lv_obj_set_pos(b3, TERM_BTN_X + TERM_BTN_PITCH * 2, 0);
    lv_obj_clear_flag(b3, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(b3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b3, lv_color_hex(0x8A4B12), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *l3 = lv_label_create(b3);
    lv_label_set_text(l3, "清除日志");
    lv_obj_set_style_text_font(l3, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(l3);
    lv_obj_add_event_cb(b3, btn_clear_logs_cb, LV_EVENT_CLICKED, NULL);

    // LiOS 第二期：相机 → LiOS 桌面的入口（放在这个隐藏终端里，不改任何现有操作习惯）
    lv_obj_t *b4 = lv_btn_create(root);
    lv_obj_set_size(b4, 60, 26);
    lv_obj_set_pos(b4, TERM_BTN_X + TERM_BTN_PITCH * 3, 0);
    lv_obj_clear_flag(b4, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(b4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b4, lv_color_hex(0x2F5D8A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *l4 = lv_label_create(b4);
    lv_label_set_text(l4, "进LiOS");
    lv_obj_set_style_text_font(l4, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(l4);
    lv_obj_add_event_cb(b4, btn_lios_cb, LV_EVENT_CLICKED, NULL);

    out_cont = lv_obj_create(root);
    lv_obj_set_size(out_cont, TERM_CARD_W - 8, 60);
    lv_obj_set_pos(out_cont, 0, 28);
    lv_obj_set_flex_flow(out_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(out_cont, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(out_cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(out_cont, lv_color_hex(0x0B1116), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(out_cont, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(out_cont, LV_SCROLLBAR_MODE_AUTO);
    out_append("就绪。例：uname -a / df -h / ls /mnt/UDISK / sh /mnt/UDISK/fixusb.sh");

    kb = ui_kb_create(root, 96, TERM_CARD_W - 8, "执行", kb_action, NULL);

    ui_trace("terminal: 控件建完，开始动画");
    card_term.show(CARD_ANIM_FLY_UP);
    term_open = true;
    if (kb)
        ui_kb_focus(kb);
    ui_trace("terminal: show 完成");
}

void menu_terminal_hide(void)
{
    if (!term_open)
        return;
    // del 会动画删除并把 obj 置空 —— 这样下次还能再 create（之前用 hide 导致进不去第二次）
    card_term.del(CARD_ANIM_FLY_DOWN);
    term_open = false;
    kb = NULL;
    out_cont = NULL;
}

bool menu_terminal_is_open(void)
{
    return term_open;
}

// 返回键：回系统设置，并且必须把模式还原（否则菜单键会失效、卡在系统设置页）
void menu_terminal_loop(bool go_back)
{
    if (!term_open)
        return;
    if (go_back)
    {
        menu_terminal_hide();
        menu_system_show();
        current_mode = MODE_SYSTEM_SETTINGS;
        ui_trace("terminal: 退出，current_mode 还原为 MODE_SYSTEM_SETTINGS");
    }
}
