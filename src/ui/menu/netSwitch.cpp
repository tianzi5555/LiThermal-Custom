// 隐藏的网络功能开关页
//
// 进入方式：系统设置 → 滚轮选到「手动清除噪声」→ 按录像键（F3）
// 作用：总开关。默认关闭；关闭时完全不碰 USB（保持原生 adb+MTP），相机功能正常。
//       开启后才在主菜单出现「WiFi 设置」入口，此时 USB 口变成 ECM 网卡，
//       相机的热像功能会因为 USB 口被占用而不可用（这是设计取舍）。
//
// 开关状态存在 /mnt/UDISK/wifi_enabled，开机脚本 usbnet_boot.sh 会读它。

#include "my_main.h"
#include "wifiManager.h"

#define NS_CARD_W 304
#define NS_CARD_H 196
#define NS_CARD_Y 22

static MyCard card_netswitch;
static bool ns_open = false;
static lv_obj_t *lbl_ns_state = NULL;
static lv_obj_t *lbl_ns_hint = NULL;
static lv_obj_t *sw_ns = NULL;

static void ns_update_state_text(void)
{
    int on = net_feature_enabled();
    if (lbl_ns_state && lv_obj_is_valid(lbl_ns_state))
    {
        if (on)
            lv_label_set_text(lbl_ns_state, "当前：已启用（重启后按网卡模式工作）");
        else
            lv_label_set_text(lbl_ns_state, "当前：已关闭（USB 口保持原生，相机功能正常）");
        lv_obj_set_style_text_color(lbl_ns_state,
                                    lv_color_hex(on ? 0x3DDC97 : 0xE8F1FF),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void ns_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    net_feature_set(on);
    ns_update_state_text();
}

static void ns_reboot_cb(lv_event_t *e)
{
    (void)e;
    if (lbl_ns_hint && lv_obj_is_valid(lbl_ns_hint))
        lv_label_set_text(lbl_ns_hint, "正在重启…");
    sync();
    system("reboot");
}

static void ns_close_cb(lv_event_t *e)
{
    (void)e;
    menu_netswitch_hide();
    menu_system_show();
    current_mode = MODE_SYSTEM_SETTINGS;
}

void menu_netswitch_show(void)
{
    if (ns_open)
        return;
    card_netswitch.create(lv_layer_top(), 0, NS_CARD_Y, NS_CARD_W, NS_CARD_H, LV_ALIGN_TOP_MID);
    lv_obj_t *root = card_netswitch.obj;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(root, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(root, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_pos(title, 2, 0);
    lv_label_set_text(title, "网络功能开关");
    lv_obj_set_style_text_font(title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 开关行
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_set_size(row, NS_CARD_W - 24, 44);
    lv_obj_set_pos(row, 0, 24);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lab = lv_label_create(row);
    lv_obj_set_align(lab, LV_ALIGN_LEFT_MID);
    lv_label_set_text(lab, "启用 WiFi 网卡");
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    sw_ns = lv_switch_create(row);
    lv_obj_set_size(sw_ns, 56, 28);
    lv_obj_set_align(sw_ns, LV_ALIGN_RIGHT_MID);
    if (net_feature_enabled())
        lv_obj_add_state(sw_ns, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_ns, ns_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lbl_ns_state = lv_label_create(root);
    lv_obj_set_pos(lbl_ns_state, 2, 72);
    lv_obj_set_width(lbl_ns_state, NS_CARD_W - 24);
    lv_obj_set_style_text_font(lbl_ns_state, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lbl_ns_hint = lv_label_create(root);
    lv_obj_set_pos(lbl_ns_hint, 2, 92);
    lv_obj_set_width(lbl_ns_hint, NS_CARD_W - 24);
    lv_label_set_long_mode(lbl_ns_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_ns_hint,
                      "开启后：USB 口变成 ECM 网卡，热像功能不可用，需重启生效。\n"
                      "关闭后：USB 口恢复原生 adb+MTP，相机功能完全正常。");
    lv_obj_set_style_text_font(lbl_ns_hint, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_ns_hint, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *b_reboot = lv_btn_create(root);
    lv_obj_set_size(b_reboot, 130, 34);
    lv_obj_set_pos(b_reboot, 2, 150);
    lv_obj_add_flag(b_reboot, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t *l1 = lv_label_create(b_reboot);
    lv_obj_set_align(l1, LV_ALIGN_CENTER);
    lv_label_set_text(l1, "立即重启生效");
    lv_obj_set_style_text_font(l1, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(b_reboot, ns_reboot_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_close = lv_btn_create(root);
    lv_obj_set_size(b_close, 100, 34);
    lv_obj_set_pos(b_close, 150, 150);
    lv_obj_add_flag(b_close, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t *l2 = lv_label_create(b_close);
    lv_obj_set_align(l2, LV_ALIGN_CENTER);
    lv_label_set_text(l2, "返回");
    lv_obj_set_style_text_font(l2, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(b_close, ns_close_cb, LV_EVENT_CLICKED, NULL);

    ns_update_state_text();
    card_netswitch.show(CARD_ANIM_FLY_UP);
    ns_open = true;
    // 默认把焦点放在开关上，转轮子/按一下就能切
    lv_group_focus_obj(sw_ns);
}

void menu_netswitch_hide(void)
{
    if (!ns_open)
        return;
    sw_ns = NULL;
    lbl_ns_state = NULL;
    lbl_ns_hint = NULL;
    card_netswitch.del(CARD_ANIM_FLY_DOWN);
    ns_open = false;
}

bool menu_netswitch_is_open(void)
{
    return ns_open;
}

void menu_netswitch_loop(bool go_back)
{
    if (!ns_open)
        return;
    if (go_back)
    {
        LOCKLV();
        menu_netswitch_hide();
        menu_system_show();
        current_mode = MODE_SYSTEM_SETTINGS;
        UNLOCKLV();
    }
    HAL::key_press_event[3] = false;
}
