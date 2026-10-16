// LiOS 第一期：LiOS 模式的占位桌面
//
// 这一期只做「切进去能看到东西 + 能按菜单键回相机」：
//   黑底 + 一行「LiOS 桌面（建设中）」+ 一个可用滚轮选择、确认进入的方块
//   （方块确认 = 回相机；菜单键也直接回相机）
//
// 真正的桌面（方块网格式应用列表、扫描 apps/*.so、dlopen 插件）放在第二期，
// 所以这里的布局故意做成「以后直接换成网格」的形状。
//
// 线程说明：
//   * 进入本函数时 UI 线程（HAL::lv_loop）已经在跑，所以本函数**不能**再自己调
//     lv_timer_handler()，只负责建界面 + 等按键。
//   * 界面建在 lv_layer_sys 上（全屏不透明，盖住相机界面），退出时整块删掉，
//     相机界面原封不动，不需要动画/切屏，也就没有切屏删除、焦点残留这些坑。
//   * 所有 LVGL 操作都在 LOCKLV()/UNLOCKLV() 里（HAL 线程也会碰 LVGL）。

#include "lios_user.h"
#include "my_main.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t *scr_lios = NULL;
static lv_obj_t *box_app = NULL; // 桌面上的方块（滚轮可选择、确认可进入）
static volatile bool lios_desktop_done = false;

static void desktop_update_box(void)
{
    if (box_app == NULL || !lv_obj_is_valid(box_app))
        return;
    lv_obj_set_style_border_width(box_app, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(box_app, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(box_app);
    lv_group_focus_obj(box_app);
}

// 方块被确认（滚轮按下）：回相机
static void desktop_box_cb(lv_event_t *e)
{
    (void)e;
    lios_log("桌面方块确认 -> 回相机");
    lios_desktop_done = true;
}

static void desktop_cleanup(void)
{
    LOCKLV();
    if (box_app != NULL && lv_obj_is_valid(box_app))
        lv_group_focus_obj(box_app); // 先把焦点挪走，别留在马上要删的对象上
    if (scr_lios != NULL && lv_obj_is_valid(scr_lios))
        lv_obj_del(scr_lios);
    scr_lios = NULL;
    box_app = NULL;
    UNLOCKLV();
}

void lios_launcher_screen(void)
{
    lios_log("进入 LiOS 桌面（占位）");

    LOCKLV();
    scr_lios = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(scr_lios, 320, 240);
    lv_obj_set_pos(scr_lios, 0, 0);
    lv_obj_clear_flag(scr_lios, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr_lios, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(scr_lios, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(scr_lios, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scr_lios, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr_lios, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(scr_lios);
    lv_label_set_text(title, "LiOS 桌面（建设中）");
    lv_obj_set_style_text_font(title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    // 方块（本期只有一个；第二期换成方块网格 + 扫描 apps/*.so）
    box_app = lv_btn_create(scr_lios);
    lv_obj_set_size(box_app, 92, 92);
    lv_obj_align(box_app, LV_ALIGN_CENTER, 0, 6);
    lv_obj_clear_flag(box_app, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(box_app, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(box_app, lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *bl1 = lv_label_create(box_app);
    lv_label_set_text(bl1, "回相机");
    lv_obj_set_style_text_font(bl1, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(bl1, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(bl1, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *bl2 = lv_label_create(box_app);
    lv_label_set_text(bl2, "滚轮选择\n确认进入");
    lv_obj_set_style_text_font(bl2, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(bl2, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(bl2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(bl2, LV_ALIGN_CENTER, 0, 22);

    lv_obj_add_event_cb(box_app, desktop_box_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(scr_lios);
    lv_label_set_text(hint, "菜单键返回相机");
    lv_obj_set_style_text_font(hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_move_foreground(scr_lios); // 状态条等 sys 层元素也要被盖住
    desktop_update_box();
    UNLOCKLV();

    lios_desktop_done = false;
    while (!lios_desktop_done)
    {
        // 菜单键：直接回相机（和方块确认等效）
        bool menu = false;
        LOCKLV();
        if (HAL::key_press_event[1])
        {
            HAL::key_press_event[1] = false;
            menu = true;
        }
        UNLOCKLV();
        if (menu)
        {
            lios_log("菜单键 -> 回相机");
            lios_desktop_done = true;
        }
        usleep(20000);
    }

    desktop_cleanup();
    lios_log("退出 LiOS 桌面");
}
