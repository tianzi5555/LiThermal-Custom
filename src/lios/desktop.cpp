// LiOS 桌面 —— 方块网格 + 页面栈 + 返回键
//
// 规则（用户要求：每个页面都要能回上一级，绝不死机）：
//   * 桌面（网格）：滚轮移动高亮，波轮按下 = 进入方块；菜单键无上级（给提示，不崩）
//   * 应用页：菜单键 / 录像键 / 波轮按下 都回桌面
//
// 线程：本文件所有函数都在 UI 线程、且调用者已持锁（LOCKLV）时执行。
// 所以这里**绝不能再 LOCKLV()**（递归锁虽然兜底，但别依赖它），也**绝不做文件 I/O**。

#include "lios.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GRID_COLS 3
#define TILE_W 84
#define TILE_H 60
#define TILE_GAP_X 4
#define TILE_GAP_Y 4
#define GRID_X 30 // 3*84 + 2*4 = 260；(320-260)/2 = 30
#define GRID_Y 26
#define GRID_H 208

static lv_obj_t *s_grid = NULL;  // 网格容器
static lv_obj_t *s_hint = NULL;  // 底部提示行
static lv_obj_t *s_page = NULL;  // 当前应用页（NULL = 在网格）
static int s_page_idx = -1;
static bool s_page_wheel_focus = false;              // 当前页面是否要 LVGL 滚轮焦点
static const lios_page_hooks_t *s_page_hooks = NULL; // 当前页面的钩子（真页面用）

void lios_page_set_hooks(const lios_page_hooks_t *hooks)
{
    s_page_hooks = hooks;
}

// 打开第 idx 个应用的页面（UI 线程、已持锁）
static void desktop_open(int idx);

// ---------------------------------------------------------------- 提示行

static void set_hint(const char *s)
{
    if (s_hint != NULL && lv_obj_is_valid(s_hint))
        lv_label_set_text(s_hint, s ? s : "");
}

// ---------------------------------------------------------------- 进入 / 退出方块

static void tile_click_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(t);
    desktop_open(idx);
}

// 「建设中」占位页（批 2/3/4 会把它们换成真页面）
static lv_obj_t *placeholder_create(lv_obj_t *parent, const char *name)
{
    lv_obj_t *t1 = lv_label_create(parent);
    lv_label_set_text(t1, name);
    lv_obj_set_style_text_font(t1, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t1, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *t2 = lv_label_create(parent);
    lv_label_set_text(t2, "建设中");
    lv_obj_set_style_text_font(t2, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t2, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(t2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *t3 = lv_label_create(parent);
    lv_label_set_text(t3, "菜单键 / 录像键 返回桌面");
    lv_obj_set_style_text_font(t3, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t3, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(t3, LV_ALIGN_BOTTOM_MID, 0, -8);
    return t1;
}

static void desktop_open(int idx)
{
    if (idx < 0 || idx >= LIOS_APP_COUNT)
        return;
    const lios_app_def_t *app = &LIOS_APPS[idx];

    // 「相机」= fork + exec 真正的相机程序（不是页面）
    if (strcmp(app->id, "camera") == 0)
    {
        lios_log("桌面：点「相机」-> 启动相机程序");
        set_hint("正在启动相机...");
        lios_launch_camera();
        return;
    }

    if (s_page != NULL)
        return;

    lios_log("桌面：打开应用 %s", app->id);
    s_page_hooks = NULL;
    s_page_wheel_focus = app->wheel_focus;
    s_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_page, 320, 240);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_page, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(s_page);

    // 列表类页面（wheel_focus=true）自己用滚轮走焦点；占位页关掉编码器输入，
    // 免得滚轮/确认去动背后桌面上的方块
    lv_indev_enable(NULL, app->wheel_focus);

    if (app->create != NULL)
        app->create(s_page);
    else
        placeholder_create(s_page, app->name);

    s_page_idx = idx;
    set_hint("菜单键 / 录像键 返回桌面");
    lv_obj_invalidate(s_page);
}

// 退出当前页面（UI 线程、已持锁）
static void desktop_close_page(void)
{
    if (s_page == NULL)
        return;
    lios_log("桌面：关闭应用页 %s", (s_page_idx >= 0 && s_page_idx < LIOS_APP_COUNT)
                                        ? LIOS_APPS[s_page_idx].id
                                        : "?");
    if (s_page_hooks != NULL && s_page_hooks->destroy != NULL)
        s_page_hooks->destroy();
    s_page_hooks = NULL;
    s_page_wheel_focus = false;
    if (lv_obj_is_valid(s_page))
        lv_obj_del(s_page);
    s_page = NULL;
    s_page_idx = -1;
    lv_indev_enable(NULL, true); // 恢复滚轮走焦点
    set_hint("滚轮选择  按下进入  相机在第一个方块");
}

// ---------------------------------------------------------------- 桌面创建

void lios_desktop_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(scr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LiOS");
    lv_obj_set_style_text_font(title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title, 6, 4);

    s_grid = lv_obj_create(scr);
    lv_obj_set_size(s_grid, 320, GRID_H);
    lv_obj_set_pos(s_grid, 0, GRID_Y);
    lv_obj_set_style_bg_opa(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < LIOS_APP_COUNT; i++)
    {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        lv_obj_t *t = lv_btn_create(s_grid);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_pos(t, GRID_X + col * (TILE_W + TILE_GAP_X),
                       row * (TILE_H + TILE_GAP_Y) + 4);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_radius(t, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(t, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(t, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *ic = lv_label_create(t);
        lv_label_set_text(ic, LIOS_APPS[i].icon);
        lv_obj_set_style_text_font(ic, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ic, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);

        lv_obj_t *nm = lv_label_create(t);
        lv_label_set_text(nm, LIOS_APPS[i].name);
        lv_obj_set_style_text_font(nm, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -8);

        lv_obj_set_user_data(t, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED, NULL);

        if (i == 0)
            lv_group_focus_obj(t);
    }

    s_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(s_hint, "滚轮选择  按下进入  相机在第一个方块");

    lios_log("桌面：已建好 %d 个方块", LIOS_APP_COUNT);
}

// ---------------------------------------------------------------- 每帧逻辑

void lios_desktop_tick(void)
{
    // 电源键长按 -> 关机（要碰文件/串口，所以放这里而不是 LVGL 回调里）
    if (lios_power_key_check())
        return;

    // 页面钩子每帧（真页面在这里取 worker 线程的结果、更新界面）
    if (s_page != NULL && s_page_hooks != NULL && s_page_hooks->tick != NULL)
        s_page_hooks->tick();

    bool menu = lios_key_take(LIOS_K_MENU);
    bool rec = lios_key_take(LIOS_K_REC);
    bool enter = lios_key_take(LIOS_K_ENTER);

    if (s_page != NULL)
    {
        // 先给页面自己处理的机会（真页面：文件管理器要「上/下级」「操作菜单」）
        if (s_page_hooks != NULL)
        {
            if (menu && s_page_hooks->key != NULL && s_page_hooks->key(LIOS_K_MENU))
                menu = false;
            if (rec && s_page_hooks->key != NULL && s_page_hooks->key(LIOS_K_REC))
                rec = false;
            if (enter && s_page_hooks->key != NULL && s_page_hooks->key(LIOS_K_ENTER))
                enter = false;
        }
        else
        {
            // 页面没消费掉的确认键：
            //   * 需要滚轮焦点的页面（列表类）：交给 LVGL 点击，桌面不抢
            //   * 占位页：当作「返回」
            if (enter && s_page_wheel_focus)
                enter = false;
        }

        if (menu || rec || enter)
        {
            lios_log("返回：%s -> 回桌面", menu ? "菜单键" : (rec ? "录像键" : "确认键"));
            desktop_close_page();
        }
        return;
    }

    // 桌面上：菜单键没有上一级；录像键也没动作（确认键由 LVGL 用来点方块）
    if (menu)
        set_hint("已经在桌面（相机是第一个方块）");
    else if (rec)
        set_hint("桌面上录像键无动作");
}

void lios_desktop_redraw_all(void)
{
    lv_obj_t *scr = lv_scr_act();
    if (scr != NULL)
        lv_obj_invalidate(scr);
    if (lv_layer_top() != NULL)
        lv_obj_invalidate(lv_layer_top());
    if (lv_layer_sys() != NULL)
        lv_obj_invalidate(lv_layer_sys());
    set_hint("滚轮选择  按下进入  相机在第一个方块");
    lios_log("桌面：相机已退出，整屏重绘");
}

bool lios_desktop_page_open(void)
{
    return s_page != NULL;
}
