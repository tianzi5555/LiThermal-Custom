// WiFi 网络设置界面（相机侧）
//
// 交互：相机只有一个旋钮 + 3 个按键，所以软键盘做成「一个键一个按钮」，靠 LVGL 默认焦点
//       轮转工作（旋钮转动=换键/换控件，旋钮按下=触发），不依赖编辑模式，最稳。
//       电源键=退出，菜单键=返回（列表页按一下关闭，密码页按一下回列表）。
//
// 页面：
//   列表页：扫到的网络（SSID / 信号 / 是否加密）+ [Rescan] [Manual] [Close]
//   密码页：密码显示框 + 字符计数 + 40 键软键盘（abc / ABC / !@# 三页）+ 功能键行
//
// 注意：相机里的中文字体是子集化过的（只含 0x20-0x7F 和 107 个特定汉字），
//       所以这里所有文案一律用 ASCII，绝对不会出现空心方块。
//
// 后端：wifiManager（BW21 控制 API / 本机 wlan0 / 演示数据）

#include "my_main.h"
#include "uiTrace.h"
#include "wifiManager.h"

#define WIFI_CARD_W 314
#define WIFI_CARD_H 236
#define WIFI_CARD_Y 2

#define PAGE_Y 40
#define PAGE_W (WIFI_CARD_W - 12)
#define PAGE_H 192

#define KB_ROWS 4
#define KB_COLS 10
#define KB_KEY_W 28
#define KB_KEY_H 28
#define KB_PITCH_X 29
#define KB_PITCH_Y 30
#define KB_Y0 46

static MyCard card_wifi;
static bool wifi_open = false;
static bool pass_open = false;
static bool wifi_inited = false;

static lv_obj_t *page_list = NULL;
static lv_obj_t *page_pass = NULL;
static lv_obj_t *list_cont = NULL;
static lv_obj_t *lbl_title = NULL;
static lv_obj_t *lbl_status = NULL;
static lv_obj_t *lbl_len = NULL;
static lv_obj_t *ta_pass = NULL;
static lv_obj_t *btn_page = NULL;
static lv_obj_t *kb_keys[KB_ROWS * KB_COLS];

static char cur_ssid[WIFI_SSID_LEN] = "";
static int pass_mode = 0; // 0=输入密码 1=输入 SSID（手动添加隐藏网络用同一个键盘）
static int kb_page = 0;   // 0=abc+digits 1=ABC+symbols 2=symbols
static uint32_t shown_scan_seq = 0;
static uint32_t shown_status_seq = 0;
static uint32_t last_ui_ms = 0;
static uint32_t last_poll_ms = 0;

// ---------------------------------------------------------------- 键盘字符表
static const char *KB_PAGE0[KB_ROWS * KB_COLS] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
    "k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
    "u", "v", "w", "x", "y", "z", "1", "2", "3", "4",
    "5", "6", "7", "8", "9", "0", ".", "_", "-", "@"};
static const char *KB_PAGE1[KB_ROWS * KB_COLS] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "!", "#", "$", "%",
    "&", "*", "(", ")", "+", "=", ";", ":", "'", "\""};
static const char *KB_PAGE2[KB_ROWS * KB_COLS] = {
    "~", "|", "\\", "/", "?", ",", "<", ">", "[", "]",
    "{", "}", "`", "^", "%", "&", "*", "(", ")", "+",
    "=", ";", ":", "'", "\"", "!", "#", "$", "_", "-",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
static const char *const *kb_page_map(int page)
{
    if (page == 1)
        return KB_PAGE1;
    if (page == 2)
        return KB_PAGE2;
    return KB_PAGE0;
}
static const char *kb_page_name(int page)
{
    if (page == 1)
        return "大写";
    if (page == 2)
        return "符号";
    return "小写";
}

// ---------------------------------------------------------------- 小工具
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x282B30), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0081B3), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_align(lab, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    return btn;
}

static void set_btn_text(lv_obj_t *btn, const char *text)
{
    if (!btn || !lv_obj_is_valid(btn))
        return;
    lv_obj_t *lab = lv_obj_get_child(btn, 0);
    if (lab && lv_obj_is_valid(lab))
        lv_label_set_text(lab, text);
}

static void hide_page(lv_obj_t *p, bool hide)
{
    if (!p || !lv_obj_is_valid(p))
        return;
    if (hide)
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- 密码页
static void pass_update_len(void)
{
    if (lbl_len && lv_obj_is_valid(lbl_len) && ta_pass && lv_obj_is_valid(ta_pass))
        lv_label_set_text_fmt(lbl_len, "已输入 %d 个字符", (int)strlen(lv_textarea_get_text(ta_pass)));
}

static void kb_key_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= KB_ROWS * KB_COLS)
        return;
    const char *ch = kb_page_map(kb_page)[idx];
    if (ta_pass && lv_obj_is_valid(ta_pass) && ch)
        lv_textarea_add_text(ta_pass, ch);
    pass_update_len();
}

static void kb_page_cb(lv_event_t *e)
{
    (void)e;
    kb_page = (kb_page + 1) % 3;
    const char *const *map = kb_page_map(kb_page);
    for (int i = 0; i < KB_ROWS * KB_COLS; i++)
        set_btn_text(kb_keys[i], map[i]);
    set_btn_text(btn_page, kb_page_name(kb_page));
}

static void pass_back_cb(lv_event_t *e)
{
    (void)e;
    hide_page(page_pass, true);
    hide_page(page_list, false);
    pass_open = false;
    if (lbl_title && lv_obj_is_valid(lbl_title))
        lv_label_set_text(lbl_title, "WiFi 网络");
    if (list_cont && lv_obj_is_valid(list_cont) && lv_obj_get_child_cnt(list_cont) > 0)
        lv_group_focus_obj(lv_obj_get_child(list_cont, 0));
}

static void pass_connect_cb(lv_event_t *e)
{
    (void)e;
    if (!ta_pass || !lv_obj_is_valid(ta_pass))
        return;
    const char *txt = lv_textarea_get_text(ta_pass);
    // pass_mode==1：这一页其实是在输入 SSID（手动添加隐藏网络），输完再进密码页
    if (pass_mode == 1)
    {
        if (txt && txt[0])
        {
            strncpy(cur_ssid, txt, WIFI_SSID_LEN - 1);
            cur_ssid[WIFI_SSID_LEN - 1] = 0;
            pass_mode = 0;
            lv_textarea_set_text(ta_pass, "");
            pass_update_len();
            if (lbl_title && lv_obj_is_valid(lbl_title))
                lv_label_set_text_fmt(lbl_title, "密码：%.24s", cur_ssid);
        }
        return;
    }
    if (lbl_status && lv_obj_is_valid(lbl_status))
        lv_label_set_text(lbl_status, "正在把密码交给 BW21 ...");
    wifi_connect(cur_ssid, txt ? txt : "");
    pass_back_cb(NULL);
}

static void kb_space_cb(lv_event_t *e)
{
    (void)e;
    if (ta_pass && lv_obj_is_valid(ta_pass))
        lv_textarea_add_text(ta_pass, " ");
    pass_update_len();
}

static void kb_backspace_cb(lv_event_t *e)
{
    (void)e;
    if (ta_pass && lv_obj_is_valid(ta_pass))
        lv_textarea_del_char(ta_pass);
    pass_update_len();
}

static void kb_clear_cb(lv_event_t *e)
{
    (void)e;
    if (ta_pass && lv_obj_is_valid(ta_pass))
        lv_textarea_set_text(ta_pass, "");
    pass_update_len();
}

static void pass_page_build(void)
{
    if (page_pass && lv_obj_is_valid(page_pass))
    {
        hide_page(page_pass, false);
        return;
    }
    page_pass = lv_obj_create(card_wifi.obj);
    lv_obj_set_size(page_pass, PAGE_W, PAGE_H);
    lv_obj_set_pos(page_pass, 2, PAGE_Y);
    lv_obj_clear_flag(page_pass, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(page_pass, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(page_pass, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(page_pass, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ta_pass = lv_textarea_create(page_pass);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_max_length(ta_pass, WIFI_PASS_LEN - 1);
    lv_obj_set_size(ta_pass, PAGE_W - 2, 30);
    lv_obj_set_pos(ta_pass, 0, 0);
    lv_obj_set_style_text_font(ta_pass, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_group_remove_obj(ta_pass); // 只作显示，脱离焦点链

    lbl_len = lv_label_create(page_pass);
    lv_obj_set_pos(lbl_len, 2, 32);
    lv_obj_set_style_text_font(lbl_len, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_len, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);

    // 40 个字符键
    kb_page = 0;
    const char *const *map = kb_page_map(kb_page);
    for (int r = 0; r < KB_ROWS; r++)
    {
        for (int c = 0; c < KB_COLS; c++)
        {
            int i = r * KB_COLS + c;
            lv_obj_t *btn = make_btn(page_pass, map[i], c * KB_PITCH_X, KB_Y0 + r * KB_PITCH_Y, KB_KEY_W, KB_KEY_H);
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, kb_key_cb, LV_EVENT_CLICKED, NULL);
            kb_keys[i] = btn;
        }
    }

    // 功能键行：abc / Space / Del / Clear / Back / Connect
    lv_coord_t y = KB_Y0 + KB_ROWS * KB_PITCH_Y;
    lv_coord_t bw = 47, gap = 1;
    lv_obj_t *b;
    b = make_btn(page_pass, kb_page_name(kb_page), 0, y, bw, 26);
    lv_obj_add_event_cb(b, kb_page_cb, LV_EVENT_CLICKED, NULL);
    btn_page = b;
    b = make_btn(page_pass, "空格", (bw + gap) * 1, y, bw, 26);
    lv_obj_add_event_cb(b, kb_space_cb, LV_EVENT_CLICKED, NULL);
    b = make_btn(page_pass, "删", (bw + gap) * 2, y, bw, 26);
    lv_obj_add_event_cb(b, kb_backspace_cb, LV_EVENT_CLICKED, NULL);
    b = make_btn(page_pass, "清空", (bw + gap) * 3, y, bw, 26);
    lv_obj_add_event_cb(b, kb_clear_cb, LV_EVENT_CLICKED, NULL);
    b = make_btn(page_pass, "返回", (bw + gap) * 4, y, bw, 26);
    lv_obj_add_event_cb(b, pass_back_cb, LV_EVENT_CLICKED, NULL);
    b = make_btn(page_pass, "连接", (bw + gap) * 5, y, bw, 26);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1F7A4D), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(b, pass_connect_cb, LV_EVENT_CLICKED, NULL);

    lv_group_focus_obj(kb_keys[0]);
}

static void pass_page_open(const char *ssid, bool secure)
{
    strncpy(cur_ssid, ssid, WIFI_SSID_LEN - 1);
    cur_ssid[WIFI_SSID_LEN - 1] = 0;
    if (!secure)
    {
        if (lbl_status && lv_obj_is_valid(lbl_status))
            lv_label_set_text(lbl_status, "开放网络，直接连接 ...");
        wifi_connect(cur_ssid, "");
        return;
    }
    hide_page(page_list, true);
    pass_page_build();
    pass_open = true;
    pass_mode = 0;
    if (lbl_title && lv_obj_is_valid(lbl_title))
        lv_label_set_text_fmt(lbl_title, "密码：%.24s", cur_ssid);
    if (ta_pass && lv_obj_is_valid(ta_pass))
        lv_textarea_set_text(ta_pass, "");
    pass_update_len();
}

// 手动添加（隐藏网络）：先用同一个键盘输 SSID，再进密码页
static void pass_page_open_manual_ssid(void)
{
    hide_page(page_list, true);
    pass_page_build();
    pass_open = true;
    pass_mode = 1;
    cur_ssid[0] = 0;
    if (lbl_title && lv_obj_is_valid(lbl_title))
        lv_label_set_text(lbl_title, "输入网络名称");
    if (ta_pass && lv_obj_is_valid(ta_pass))
        lv_textarea_set_text(ta_pass, "");
    pass_update_len();
}

// ---------------------------------------------------------------- 列表页
static void free_ap_list(void)
{
    if (!list_cont || !lv_obj_is_valid(list_cont))
        return;
    uint32_t n = lv_obj_get_child_cnt(list_cont);
    for (uint32_t i = 0; i < n; i++)
    {
        lv_obj_t *c = lv_obj_get_child(list_cont, i);
        if (c && lv_obj_check_type(c, &lv_btn_class))
        {
            void *p = lv_obj_get_user_data(c);
            if (p)
            {
                free(p);
                lv_obj_set_user_data(c, NULL);
            }
        }
    }
}

static void ap_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *ssid = (const char *)lv_obj_get_user_data(btn);
    lv_obj_t *lab = lv_obj_get_child(btn, 0);
    bool secure = true;
    if (lab && lv_obj_is_valid(lab))
    {
        const char *t = lv_label_get_text(lab);
        if (t && strstr(t, "开"))
            secure = false;
    }
    if (ssid && ssid[0])
        pass_page_open(ssid, secure);
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    if (lbl_status && lv_obj_is_valid(lbl_status))
        lv_label_set_text(lbl_status, "正在扫描 ...");
    wifi_request_scan();
}

static void manual_ssid_cb(lv_event_t *e)
{
    (void)e;
    pass_page_open_manual_ssid();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    // 事件回调运行在 UI 线程且已持锁，这里直接收，别再抢锁（会死锁）
    menu_wifi_hide();
    current_mode = MODE_MAINPAGE;
}

static void rebuild_ap_list(const wifi_state_t *st)
{
    if (!list_cont || !lv_obj_is_valid(list_cont))
        return;
    free_ap_list();
    lv_obj_clean(list_cont);
    if (st->ap_count <= 0)
    {
        lv_obj_t *lab = lv_label_create(list_cont);
        lv_label_set_text(lab, st->backend == WIFI_BACKEND_DEMO ? "暂无演示数据" : "没扫到网络，请按重新扫描");
        lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
        return;
    }
    for (int i = 0; i < st->ap_count; i++)
    {
        char text[80];
        snprintf(text, sizeof(text), "%-18.18s %4d %s",
                 st->aps[i].ssid, st->aps[i].rssi, st->aps[i].secure ? "密" : "开");
        lv_obj_t *btn = make_btn(list_cont, text, 0, 0, PAGE_W - 12, 28);
        char *ssid_copy = (char *)malloc(WIFI_SSID_LEN);
        if (ssid_copy)
        {
            strncpy(ssid_copy, st->aps[i].ssid, WIFI_SSID_LEN - 1);
            ssid_copy[WIFI_SSID_LEN - 1] = 0;
            lv_obj_set_user_data(btn, ssid_copy);
        }
        lv_obj_add_event_cb(btn, ap_btn_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_group_focus_obj(lv_obj_get_child(list_cont, 0));
}

static void update_status(const wifi_state_t *st)
{
    if (!lbl_status || !lv_obj_is_valid(lbl_status))
        return;
    lv_label_set_text(lbl_status, st->status);
    if (st->conn_state == WIFI_ST_CONNECTED)
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x3DDC97), LV_PART_MAIN | LV_STATE_DEFAULT);
    else if (st->conn_state == WIFI_ST_FAILED)
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFF5D73), LV_PART_MAIN | LV_STATE_DEFAULT);
    else
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xE8F1FF), LV_PART_MAIN | LV_STATE_DEFAULT);
}

// ---------------------------------------------------------------- 对外接口
bool menu_wifi_is_open(void)
{
    return wifi_open;
}

bool menu_wifi_password_open(void)
{
    return pass_open;
}

void menu_wifi_show(void)
{
    if (!wifi_inited)
    {
        wifi_inited = true;
        wifi_init(NULL);
    }
    if (wifi_open)
        return;

    card_wifi.create(lv_layer_top(), 0, WIFI_CARD_Y, WIFI_CARD_W, WIFI_CARD_H, LV_ALIGN_TOP_MID);
    lv_obj_t *root = card_wifi.obj;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(root, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(root, 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    lbl_title = lv_label_create(root);
    lv_obj_set_pos(lbl_title, 2, 0);
    lv_label_set_text(lbl_title, "WiFi 网络");
    lv_obj_set_style_text_font(lbl_title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lbl_status = lv_label_create(root);
    lv_obj_set_pos(lbl_status, 2, 18);
    lv_obj_set_width(lbl_status, WIFI_CARD_W - 24);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_status, "正在检测网络后端 ...");
    lv_obj_set_style_text_font(lbl_status, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 列表页
    page_list = lv_obj_create(root);
    lv_obj_set_size(page_list, PAGE_W, PAGE_H);
    lv_obj_set_pos(page_list, 2, PAGE_Y);
    lv_obj_clear_flag(page_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(page_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(page_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(page_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    list_cont = lv_obj_create(page_list);
    lv_obj_set_size(list_cont, PAGE_W - 2, 140);
    lv_obj_set_pos(list_cont, 0, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list_cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(list_cont, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(list_cont, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *b1 = make_btn(page_list, "重新扫描", 0, 146, 96, 30);
    lv_obj_t *b2 = make_btn(page_list, "手动输入", 100, 146, 96, 30);
    lv_obj_t *b3 = make_btn(page_list, "关闭", 200, 146, 96, 30);
    lv_obj_add_event_cb(b1, rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b2, manual_ssid_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b3, close_cb, LV_EVENT_CLICKED, NULL);

    card_wifi.show(CARD_ANIM_FLY_UP);
    wifi_open = true;
    pass_open = false;
    shown_scan_seq = 0;
    shown_status_seq = 0;
    last_ui_ms = 0;
    last_poll_ms = 0;
    lv_group_focus_obj(b1);
    wifi_request_scan();
    wifi_request_status();
}

void menu_wifi_hide(void)
{
    if (!wifi_open)
        return;
    pass_open = false;
    if (page_pass && lv_obj_is_valid(page_pass))
    {
        lv_obj_del(page_pass);
        page_pass = NULL;
    }
    ta_pass = NULL;
    lbl_len = NULL;
    btn_page = NULL;
    for (int i = 0; i < KB_ROWS * KB_COLS; i++)
        kb_keys[i] = NULL;
    free_ap_list();
    list_cont = NULL;
    page_list = NULL;
    lbl_title = NULL;
    lbl_status = NULL;
    card_wifi.del(CARD_ANIM_FLY_DOWN);
    wifi_open = false;
}

void menu_wifi_loop(bool go_back)
{
    if (!wifi_open)
        return;

    if (go_back)
    {
        if (pass_open)
        {
            LOCKLV();
            pass_back_cb(NULL);
            UNLOCKLV();
        }
        else
        {
            LOCKLV();
            menu_wifi_hide();
            current_mode = MODE_MAINPAGE;
            UNLOCKLV();
        }
        return;
    }

    // 录像键（F3）：列表页 = 重新扫描
    if (HAL::key_press_event[2])
    {
        HAL::key_press_event[2] = false;
        if (!pass_open)
        {
            LOCKLV();
            if (lbl_status && lv_obj_is_valid(lbl_status))
                lv_label_set_text(lbl_status, "正在扫描 ...");
            UNLOCKLV();
            wifi_request_scan();
        }
    }
    HAL::key_press_event[3] = false; // 旋钮按下交给 LVGL 处理

    uint32_t now = lv_tick_get();
    if (last_ui_ms == 0 || lv_tick_elaps(last_ui_ms) > 250)
    {
        last_ui_ms = now;
        wifi_state_t st;
        wifi_copy_state(&st);
        LOCKLV();
        if (st.scan_seq != shown_scan_seq)
        {
            shown_scan_seq = st.scan_seq;
            rebuild_ap_list(&st);
        }
        if (st.status_seq != shown_status_seq)
        {
            shown_status_seq = st.status_seq;
            update_status(&st);
        }
        if (pass_open && st.conn_state == WIFI_ST_CONNECTED)
            pass_back_cb(NULL);
        UNLOCKLV();
    }
    if (last_poll_ms == 0 || lv_tick_elaps(last_poll_ms) > 4000)
    {
        last_poll_ms = now;
        wifi_request_status();
    }
}
