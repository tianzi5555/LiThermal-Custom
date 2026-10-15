#include <my_main.h>
#include "wifiManager.h"

// WiFi 入口是否显示：由隐藏页里的「网络功能总开关」决定，默认关闭。
// 关着的时候主菜单保持原来的 3 个入口、原来的尺寸。
static bool menu_wifi_entry_enabled()
{
    return net_feature_enabled() != 0;
}
static lv_coord_t menu_box_width()
{
    return menu_wifi_entry_enabled() ? 300 : 230;
}

// 处理中键按下事件
#define MENU_BUTTON_2_BOX_WIDHT 300
#define MENU_BUTTON_2_BOX_HEIGHT 136
#define MENU_BUTTON_2_BOX_Y_SHOW 30
#define MENU_BUTTON_2_BUTTON_WIDTH_DEFAULT 60
#define MENU_BUTTON_2_BUTTON_HEIGHT_DEFAULT 100
#define MENU_BUTTON_2_BUTTON_WIDTH_FOCUSED 70
#define MENU_BUTTON_2_BUTTON_HEIGHT_FOCUSED 110
static MyCard card_menuPage;
static bool close_for_opening_submenu = false;
static int selected_menu_number = -1;
static bool expanded = false;

void event_button_clicked_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    int code = e->code;
    switch (code)
    {
    case LV_EVENT_CLICKED:
        selected_menu_number = reinterpret_cast<int>(e->user_data);
        close_for_opening_submenu = true;
        break;
    case LV_EVENT_FOCUSED:
        if (expanded)
            lv_anim_size(target, MENU_BUTTON_2_BUTTON_WIDTH_FOCUSED, MENU_BUTTON_2_BUTTON_HEIGHT_FOCUSED, 500, 0);
        break;
    case LV_EVENT_DEFOCUSED:
        if (expanded)
            lv_anim_size(target, MENU_BUTTON_2_BUTTON_WIDTH_DEFAULT, MENU_BUTTON_2_BUTTON_HEIGHT_DEFAULT, 500, 0);
        break;
    default:
        break;
    }
}

void menu_button_2_construct(lv_obj_t *parent)
{
    const bool wifi_entry = menu_wifi_entry_enabled();
    lv_obj_set_align(parent, LV_ALIGN_TOP_MID);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_bg_opa(parent, 160, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(parent, 0, 0);

    lv_obj_t *ui_Button2 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button2, MENU_BUTTON_2_BUTTON_WIDTH_DEFAULT);
    lv_obj_set_height(ui_Button2, MENU_BUTTON_2_BUTTON_HEIGHT_DEFAULT);
    lv_obj_set_x(ui_Button2, wifi_entry ? -102 : -69);
    lv_obj_set_y(ui_Button2, 2);
    lv_obj_set_align(ui_Button2, LV_ALIGN_TOP_MID);
    lv_obj_add_flag(ui_Button2, LV_OBJ_FLAG_SCROLL_ON_FOCUS); /// Flags
    lv_obj_clear_flag(ui_Button2, LV_OBJ_FLAG_SCROLLABLE);    /// Flags
    lv_obj_set_style_bg_color(ui_Button2, lv_color_hex(0x282B30), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Button2, lv_color_hex(0x0081B3), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(ui_Button2, 255, LV_PART_MAIN | LV_STATE_FOCUSED);

    lv_obj_t *ui_Label4 = lv_label_create(ui_Button2);
    lv_obj_set_width(ui_Label4, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label4, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_Label4, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label4, "");
    lv_obj_set_style_text_font(ui_Label4, &ui_font_fontawesome24, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ui_Label5 = lv_label_create(ui_Button2);
    lv_obj_set_width(ui_Label5, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label5, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_Label5, 0);
    lv_obj_set_y(ui_Label5, 40);
    lv_obj_set_align(ui_Label5, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label5, "相册");
    lv_obj_set_style_text_font(ui_Label5, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ui_Button3 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button3, MENU_BUTTON_2_BUTTON_WIDTH_DEFAULT);
    lv_obj_set_height(ui_Button3, MENU_BUTTON_2_BUTTON_HEIGHT_DEFAULT);
    lv_obj_set_x(ui_Button3, wifi_entry ? -34 : -1);
    lv_obj_set_y(ui_Button3, 3);
    lv_obj_set_align(ui_Button3, LV_ALIGN_TOP_MID);
    lv_obj_add_flag(ui_Button3, LV_OBJ_FLAG_SCROLL_ON_FOCUS); /// Flags
    lv_obj_clear_flag(ui_Button3, LV_OBJ_FLAG_SCROLLABLE);    /// Flags
    lv_obj_set_style_bg_color(ui_Button3, lv_color_hex(0x282B30), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Button3, lv_color_hex(0xBB9100), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(ui_Button3, 255, LV_PART_MAIN | LV_STATE_FOCUSED);

    lv_obj_t *ui_Label6 = lv_label_create(ui_Button3);
    lv_obj_set_width(ui_Label6, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label6, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_Label6, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label6, "");
    lv_obj_set_style_text_font(ui_Label6, &ui_font_fontawesome24, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ui_Label7 = lv_label_create(ui_Button3);
    lv_obj_set_width(ui_Label7, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label7, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_Label7, 0);
    lv_obj_set_y(ui_Label7, 40);
    lv_obj_set_align(ui_Label7, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label7, "相机\n设置");
    lv_obj_set_style_text_font(ui_Label7, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ui_Button4 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button4, MENU_BUTTON_2_BUTTON_WIDTH_DEFAULT);
    lv_obj_set_height(ui_Button4, MENU_BUTTON_2_BUTTON_HEIGHT_DEFAULT);
    lv_obj_set_x(ui_Button4, wifi_entry ? 34 : 67);
    lv_obj_set_y(ui_Button4, 3);
    lv_obj_set_align(ui_Button4, LV_ALIGN_TOP_MID);
    lv_obj_add_flag(ui_Button4, LV_OBJ_FLAG_SCROLL_ON_FOCUS); /// Flags
    lv_obj_clear_flag(ui_Button4, LV_OBJ_FLAG_SCROLLABLE);    /// Flags
    lv_obj_set_style_bg_color(ui_Button4, lv_color_hex(0x282B30), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button4, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Button4, lv_color_hex(0x960000), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(ui_Button4, 255, LV_PART_MAIN | LV_STATE_FOCUSED);

    lv_obj_t *ui_Label8 = lv_label_create(ui_Button4);
    lv_obj_set_width(ui_Label8, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label8, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_Label8, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label8, "");
    lv_obj_set_style_text_font(ui_Label8, &ui_font_fontawesome24, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ui_Label9 = lv_label_create(ui_Button4);
    lv_obj_set_width(ui_Label9, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label9, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_Label9, 0);
    lv_obj_set_y(ui_Label9, 40);
    lv_obj_set_align(ui_Label9, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label9, "系统\n设置");
    lv_obj_set_style_text_font(ui_Label9, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 第 4 个入口：WiFi 网络设置（只有总开关打开时才出现；图标位用文字，避免动字体子集）
    if (wifi_entry)
    {
    lv_obj_t *ui_Button5 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button5, MENU_BUTTON_2_BUTTON_WIDTH_DEFAULT);
    lv_obj_set_height(ui_Button5, MENU_BUTTON_2_BUTTON_HEIGHT_DEFAULT);
    lv_obj_set_x(ui_Button5, 102);
    lv_obj_set_y(ui_Button5, 3);
    lv_obj_set_align(ui_Button5, LV_ALIGN_TOP_MID);
    lv_obj_add_flag(ui_Button5, LV_OBJ_FLAG_SCROLL_ON_FOCUS); /// Flags
    lv_obj_clear_flag(ui_Button5, LV_OBJ_FLAG_SCROLLABLE);    /// Flags
    lv_obj_set_style_bg_color(ui_Button5, lv_color_hex(0x282B30), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button5, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Button5, lv_color_hex(0x1F7A4D), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(ui_Button5, 255, LV_PART_MAIN | LV_STATE_FOCUSED);

    lv_obj_t *ui_Label10 = lv_label_create(ui_Button5);
    lv_obj_set_width(ui_Label10, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label10, LV_SIZE_CONTENT); /// 1
    lv_obj_set_align(ui_Label10, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label10, "WiFi");
    lv_obj_set_style_text_font(ui_Label10, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ui_Label11 = lv_label_create(ui_Button5);
    lv_obj_set_width(ui_Label11, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height(ui_Label11, LV_SIZE_CONTENT); /// 1
    lv_obj_set_x(ui_Label11, 0);
    lv_obj_set_y(ui_Label11, 40);
    lv_obj_set_align(ui_Label11, LV_ALIGN_TOP_MID);
    lv_label_set_text(ui_Label11, "设置");
    lv_obj_set_style_text_font(ui_Label11, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_Button5, event_button_clicked_cb, LV_EVENT_ALL, reinterpret_cast<void *>(3));
    } // if (wifi_entry)

    // events（原有 3 个入口的注册必须在 if 外面，否则关闭 WiFi 功能时菜单会失灵）
    lv_obj_add_event_cb(ui_Button2, event_button_clicked_cb, LV_EVENT_ALL, reinterpret_cast<void *>(0));
    lv_obj_add_event_cb(ui_Button3, event_button_clicked_cb, LV_EVENT_ALL, reinterpret_cast<void *>(1));
    lv_obj_add_event_cb(ui_Button4, event_button_clicked_cb, LV_EVENT_ALL, reinterpret_cast<void *>(2));
}

static void card_menuPage_create()
{
    if (card_menuPage.obj == NULL || lv_obj_is_valid(card_menuPage.obj) == false)
    {
        card_menuPage.create(lv_layer_sys(), -50, 0, 0, 0, LV_ALIGN_TOP_MID);
        card_menuPage.show(CARD_ANIM_NONE);
        menu_button_2_construct(card_menuPage.obj);
        lv_group_focus_obj(lv_obj_get_child(card_menuPage.obj, 0));
    }
}
void menu_gallery_show();
void menu_gallery_loop(bool has_hal_go_back_event);
void menu_basic_show();
void menu_basic_hide();
void menu_system_show();
void menu_system_hide();

void refresh_menu_key()
{
    if (crosshair_settings_is_open())
    {
        if (HAL::key_press_event[1] == true)
        {
            HAL::key_press_event[1] = false;
            LOCKLV();
            crosshair_settings_hide();
            UNLOCKLV();
        }
        return;
    }
    if (expanded == false)
    {
        if (HAL::key_press_event[1] == true)
        {
            HAL::key_press_event[1] = false;
            switch (current_mode)
            {
            case MODE_MAINPAGE:
                expanded = true;
                close_for_opening_submenu = false;
                LOCKLV();
                card_menuPage_create();
                lv_anim_size(card_menuPage.obj, menu_box_width(), MENU_BUTTON_2_BOX_HEIGHT, 500, 0);
                lv_anim_move(card_menuPage.obj, 0, MENU_BUTTON_2_BOX_Y_SHOW, 500, 0);
                current_mode = MODE_MAINMENU;
                UNLOCKLV();
                break;
            case MODE_GALLERY:
            case MODE_GALLERY_MENU:
                menu_gallery_loop(true);            // 在这里处理事件
                break;
            case MODE_CAMERA_SETTINGS:
                LOCKLV();
                menu_basic_hide();
                UNLOCKLV();
                current_mode = MODE_MAINPAGE;
                break;
            case MODE_SYSTEM_SETTINGS:
                LOCKLV();
                menu_system_hide();
                UNLOCKLV();
                current_mode = MODE_MAINPAGE;
                break;
            case MODE_WIFI_SETTINGS:
                // 交给 WiFi 页面自己处理：密码页 → 回列表；列表页 → 关闭
                menu_wifi_loop(true);
                break;
            case MODE_NET_SWITCH:
                // 隐藏的网络功能开关页：返回到系统设置
                menu_netswitch_loop(true);
                break;
            default:
                break;
            }
        }
        else
        {
            if (card_menuPage.obj != NULL && lv_obj_is_valid(card_menuPage.obj))
            {
                if (lv_obj_get_width(card_menuPage.obj) == 0)
                {
                    // 已收起，删除widget
                    LOCKLV();
                    lv_obj_del_delayed(card_menuPage.obj, 500);
                    card_menuPage.obj = NULL;
                    UNLOCKLV();
                }
            }
        }
    }
    else
    {
        if (close_for_opening_submenu || (HAL::key_press_event[1] == true))
        {
            if (close_for_opening_submenu == false)
            {
                current_mode = MODE_MAINPAGE;
            }
            else
            {
                switch (selected_menu_number)
                {
                case 0:
                    current_mode = MODE_GALLERY;
                    menu_gallery_show();        // 这里不需要锁定UI线程
                    break;
                case 1:
                    current_mode = MODE_CAMERA_SETTINGS;
                    LOCKLV();
                    menu_basic_show();
                    UNLOCKLV();
                    break;
                case 2:
                    current_mode = MODE_SYSTEM_SETTINGS;
                    LOCKLV();
                    menu_system_show();
                    UNLOCKLV();
                    break;
                case 3:
                    current_mode = MODE_WIFI_SETTINGS;
                    LOCKLV();
                    menu_wifi_show();
                    UNLOCKLV();
                    break;
                default:
                    current_mode = MODE_MAINPAGE;
                    break;
                }
            }
            selected_menu_number = -1;
            close_for_opening_submenu = false;
            HAL::key_press_event[1] = false;
            expanded = false;
            LOCKLV();
            lv_anim_size(card_menuPage.obj, 0, 0, 500, 0);
            lv_anim_move(card_menuPage.obj, -50, 0, 500, 0);
            UNLOCKLV();
        }
    }
}