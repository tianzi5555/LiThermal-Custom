// LiOS 第一期：开机密码界面 + 两种模式的入口
//
// 这一版按「同一个程序里的两种顶层模式」实现（不是两个二进制）：
//   LiThermal --login
//     → 先进密码界面（LVGL）
//         camera 密码 → LIOS_ENTER_CAMERA，调用方继续跑原来的相机初始化
//         lios   密码 → LIOS_ENTER_LIOS，调用方在 UI 线程起来后调 lios_launcher_screen()
//
// 关键点（踩过坑，务必保持）：
//   * 本文件所有 LVGL 对象的创建/修改都在 LOCKLV()/UNLOCKLV() 里。
//     虽然登录界面运行期间 UI 线程（HAL::lv_loop）还没起来，但 HAL 线程
//     （thread_hal_func → refresh_menu_key / refresh_poweroff_key）已经在跑了，
//     它也会碰 LVGL（比如关机黑屏那个 overlay），所以必须加锁。
//   * 登录期间由本文件自己跑 lv_timer_handler()（只可能一次：UI 线程还没起来），
//     绝不能让两个线程同时跑 lv_timer_handler。
//   * 任何异常/兜底（没有配置、密码读不出、连出错的回调）都返回 LIOS_ENTER_CAMERA，
//     保证「绝不锁死用户」。

#include "lios_user.h"
#include "my_main.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------- 启动参数

static bool g_login_requested = false;

bool lios_login_requested(void)
{
    return g_login_requested;
}

bool lios_user_prepare(int argc, char **argv)
{
    g_login_requested = false;
    for (int i = 1; i < argc; i++)
    {
        if (argv[i] != NULL && strcmp(argv[i], "--login") == 0)
            g_login_requested = true;
    }
    if (g_login_requested)
        printf("[lios] --login 启动：先出开机密码界面\n");
    else
        printf("[lios] 未带 --login：跳过密码界面，行为与以前完全一致\n");
    return g_login_requested;
}

// ---------------------------------------------------------------- 登录界面布局

#define LOGIN_PW_MAX 16 // 密码最大长度（默认 123456 / 5555；配置里更长的密码会输不全 → 所以给足 16 位）
#define LOGIN_W 320
#define LOGIN_H 240
#define LOGIN_GRID_X 34 // 3 列 84 宽 + 4 间距 = 260，居中：(320-260)/2 = 30，留点边距
#define LOGIN_GRID_Y 86
#define LOGIN_BTN_W 84
#define LOGIN_BTN_H 34
#define LOGIN_BTN_GAP 4
#define LOGIN_NEED_CONFIRM 6 // 连错这么多次就提示「菜单键直接进相机」

// 数字键（顺序 = LVGL 组的遍历顺序，也是滚轮顺序；正好 12 个键占满 4×3）
static const char *LOGIN_KEYS[12] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    "退回", "0", "确定"};

static lv_obj_t *scr_login = NULL;  // 登录界面根容器（全屏，放在 lv_layer_sys）
static lv_obj_t *login_ta = NULL;   // 密码输入框
static lv_obj_t *login_msg = NULL;  // 底部提示行（错误/重输提示）
static lv_obj_t *login_keys[12] = {NULL};
static int login_idx = 0; // 当前选中的键
static int login_err_cnt = 0;
static volatile int login_result = -1; // -1 继续等 / LIOS_ENTER_CAMERA / LIOS_ENTER_LIOS

static const char *LOGIN_HELP = "输完密码，滚轮移到「确定」按下";

static void login_refresh_field(void)
{
    if (login_ta == NULL || !lv_obj_is_valid(login_ta))
        return;
    const char *t = lv_textarea_get_text(login_ta);
    int n = (t != NULL) ? (int)strlen(t) : 0;
    if (login_msg != NULL && lv_obj_is_valid(login_msg))
    {
        if (login_err_cnt >= LOGIN_NEED_CONFIRM)
            lv_label_set_text(login_msg, "密码错误。菜单键可直接进相机");
        else if (n > 0)
            lv_label_set_text(login_msg, LOGIN_HELP);
        else
            lv_label_set_text(login_msg, "请输入密码（菜单键退出）");
    }
}

// 滚轮换键：自己改焦点 + 自己刷样式（不依赖 LVGL 的 FOCUSED 事件）
static void login_apply_nav(void)
{
    for (int i = 0; i < 12; i++)
    {
        lv_obj_t *b = login_keys[i];
        if (b == NULL || !lv_obj_is_valid(b))
            continue;
        if (i == login_idx)
        {
            lv_obj_set_style_border_width(b, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(b, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else
        {
            lv_obj_set_style_border_width(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    if (login_keys[login_idx] != NULL && lv_obj_is_valid(login_keys[login_idx]))
        lv_group_focus_obj(login_keys[login_idx]);
}

static void login_press_key(int idx)
{
    if (login_ta == NULL || !lv_obj_is_valid(login_ta))
        return;
    const char *k = LOGIN_KEYS[idx];
    if (strcmp(k, "退回") == 0)
        lv_textarea_del_char(login_ta);
    else if (strcmp(k, "确定") == 0)
        lv_event_send(login_keys[idx], LV_EVENT_CLICKED, NULL);
    else
    {
        const char *t = lv_textarea_get_text(login_ta);
        if (t == NULL || (int)strlen(t) < LOGIN_PW_MAX)
            lv_textarea_add_text(login_ta, k);
    }
    login_refresh_field();
}

// 12 个键点击：数字/退回/确定
static void login_key_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= 12)
        return;

    if (strcmp(LOGIN_KEYS[idx], "确定") == 0)
    {
        const char *t = lv_textarea_get_text(login_ta);
        int r = lios_login_check(t);
        if (r >= 0)
        {
            login_result = r;
            return;
        }
        // 密码错误：清空重输，绝不崩溃、不退出
        login_err_cnt++;
        lios_log("密码错误第 %d 次", login_err_cnt);
        lv_textarea_set_text(login_ta, "");
        if (login_msg != NULL && lv_obj_is_valid(login_msg))
        {
            if (login_err_cnt >= LOGIN_NEED_CONFIRM)
                lv_label_set_text(login_msg, "密码错误。菜单键可直接进相机");
            else
                lv_label_set_text(login_msg, "密码错误，请重输");
        }
        return;
    }

    login_press_key(idx);
    login_refresh_field();
}

static void login_make_key(lv_obj_t *parent, int idx)
{
    const char *txt = LOGIN_KEYS[idx];
    int col = idx % 3;
    int row = idx / 3;

    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, LOGIN_BTN_W, LOGIN_BTN_H);
    lv_obj_set_pos(b, LOGIN_GRID_X + col * (LOGIN_BTN_W + LOGIN_BTN_GAP),
                   LOGIN_GRID_Y + row * (LOGIN_BTN_H + LOGIN_BTN_GAP));
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(b, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (strcmp(txt, "确定") == 0)
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1E7F4F), LV_PART_MAIN | LV_STATE_DEFAULT);
    else if (strcmp(txt, "退回") == 0)
        lv_obj_set_style_bg_color(b, lv_color_hex(0x5A3A22), LV_PART_MAIN | LV_STATE_DEFAULT);
    else
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lab = lv_label_create(b);
    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lab);

    lv_obj_set_user_data(b, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(b, login_key_cb, LV_EVENT_CLICKED, NULL);
    login_keys[idx] = b;
}

static void login_cleanup(void)
{
    LOCKLV();
    if (login_keys[0] != NULL && lv_obj_is_valid(login_keys[0]))
        lv_group_focus_obj(login_keys[0]); // 别把焦点留在马上要删的对象上
    if (scr_login != NULL && lv_obj_is_valid(scr_login))
        lv_obj_del(scr_login);
    scr_login = NULL;
    login_ta = NULL;
    login_msg = NULL;
    for (int i = 0; i < 12; i++)
        login_keys[i] = NULL;
    login_idx = 0;
    UNLOCKLV();
}

lios_mode_t lios_login_show_and_run(void)
{
    lios_config_load();

    LOCKLV();
    scr_login = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(scr_login, LOGIN_W, LOGIN_H);
    lv_obj_set_pos(scr_login, 0, 0);
    lv_obj_clear_flag(scr_login, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr_login, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(scr_login, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(scr_login, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scr_login, lv_color_hex(0x0A0E12), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr_login, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(scr_login);
    lv_label_set_text(title, "LiThermal 开机密码");
    lv_obj_set_style_text_font(title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title, 8, 8);

    // 密码输入框：不参与焦点（这样滚轮只会走 12 个键，也不会误改输入），
    // 显示成圆点（不泄露密码），最长 4 位。
    login_ta = lv_textarea_create(scr_login);
    lv_obj_set_size(login_ta, 140, 34);
    lv_obj_set_pos(login_ta, 90, 34);
    lv_textarea_set_one_line(login_ta, true);
    lv_textarea_set_password_mode(login_ta, true);
    lv_textarea_set_max_length(login_ta, LOGIN_PW_MAX);
    lv_obj_clear_flag(login_ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(login_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_text_font(login_ta, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(login_ta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(login_ta, lv_color_hex(0x11181F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(login_ta, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(login_ta, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(login_ta, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);

    for (int i = 0; i < 12; i++)
        login_make_key(scr_login, i);

    login_msg = lv_label_create(scr_login);
    lv_obj_set_style_text_font(login_msg, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(login_msg, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(login_msg, 8, 218);
    lv_label_set_text(login_msg, "请输入密码（菜单键退出）");

    login_idx = 0;
    login_apply_nav();
    UNLOCKLV();

    lios_log("密码界面已显示");

    login_result = -1;
    // 登录期间 UI 线程还没起来，这里自己跑 LVGL 主循环（先睡一会儿让屏幕刷出来）
    usleep(50000);
    while (login_result < 0)
    {
        LOCKLV();
        lv_timer_handler();
        if (HAL::key_press_event[1])
        {
            // 菜单键 = 退出登录 = 直接进相机（硬要求：绝不锁死用户）
            HAL::key_press_event[1] = false;
            login_result = LIOS_ENTER_CAMERA;
            lios_log("菜单键退出登录，直接进相机");
        }
        UNLOCKLV();
        usleep(10000);
    }

    lios_mode_t ret = (login_result == LIOS_ENTER_LIOS) ? LIOS_ENTER_LIOS : LIOS_ENTER_CAMERA;
    lios_log("登录结束 -> %s", (ret == LIOS_ENTER_LIOS) ? "LiOS 模式" : "相机");
    login_cleanup();
    printf("[lios] 登录结果: %s\n", (ret == LIOS_ENTER_LIOS) ? "LiOS 模式" : "相机");
    return ret;
}
