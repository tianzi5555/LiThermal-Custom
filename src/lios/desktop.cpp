// LiOS 桌面 —— 方块网格（分页）+ 页面栈 + 返回键 + 插件页
//
// 规则（用户要求：每个页面都要能回上一级，绝不死机）：
//   * 桌面（网格）：滚轮移动高亮，波轮按下 = 进入方块；菜单键无上级（给提示，不崩）
//   * 桌面分页：3 列 x 3 行 = 每页 9 个方块；一页满了就往右加一页（页与页水平排列）。
//     外层容器水平可滚动，方块带 LV_OBJ_FLAG_SCROLL_ON_FOCUS →
//     焦点跨页时外层自动横滚，这就是「往右加页」的观感。
//     **录像键 = 翻到下一页（循环）**；底部有页码指示（1/2）与提示行。
//   * 应用页：菜单键 回桌面；别的键先给页面自己（真页面 / 插件）
//
// 线程：本文件所有函数都在 UI 线程、且调用者已持锁（LOCKLV）时执行。
// 所以这里**绝不能再 LOCKLV()**（递归锁虽然兜底，但别依赖它），也**绝不做文件 I/O**。
// 插件扫描在 apps.cpp 的 worker 线程里做，这里只是每帧比一下版本号。

#include "lios.h"
#include "lios_perm.h"
#include "lios_app.h" // 插件按键枚举（LIOS_KEY_* / LIOS_EV_*）

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- 网格尺寸（320x240）
#define GRID_COLS 6
#define GRID_ROWS 6
#define PER_PAGE (GRID_COLS * GRID_ROWS) // 每页 36 个（方块缩小一倍）
#define TILE_W 42
#define TILE_H 28
#define TILE_GAP_X 4
#define TILE_GAP_Y 4
#define GRID_X 24                    // 6*42 + 5*4 = 272；(320-272)/2 = 24
#define GRID_Y 24
#define GRID_H 200
#define PAGE_W 320                   // 每页宽 = 屏宽
#define PAGE_H GRID_H
#define TILE_Y0 4

static lv_obj_t *s_grid = NULL;  // 外层容器（水平可滚动，里面每页一个 320 宽的容器）
static lv_obj_t *s_pages[24] = {0};
static lv_obj_t *s_tiles[256] = {0};
static lv_obj_t *s_page_lbl = NULL; // 底部页码 1/2
static lv_obj_t *s_hint = NULL;     // 底部提示行
static lv_obj_t *s_page = NULL;     // 当前应用页（NULL = 在网格）
static int s_page_idx = -1;
static bool s_page_wheel_focus = false;              // 当前页面是否要 LVGL 滚轮焦点
static const lios_page_hooks_t *s_page_hooks = NULL; // 当前页面的钩子（真页面用）

// 插件页
static const lios_plugin_ops_t *s_plug_ops = NULL;
static int s_plug_idx = -1;
static bool s_enc_prev = false; // 上一帧波轮是否按着（给插件发 按下/抬起）

static int s_total = 0;      // 当前清单条目数（内置 + 插件）
static int s_pages_n = 0;
static int s_shown_page = 0; // 页码指示当前显示第几页（0 基）
static lv_group_t *s_grp_desktop = NULL; // 桌面主菜单的组
static int s_seen_rev = -1;  // 已看到的插件清单版本
static lv_group_t *s_grp_page = NULL;    // 当前页面的组

void lios_page_set_hooks(const lios_page_hooks_t *hooks)
{
    s_page_hooks = hooks;
}

// 打开第 idx 个应用的页面（UI 线程、已持锁）
static void page_group_enter(void)
{
    if (s_grp_desktop == NULL)
        s_grp_desktop = lv_group_get_default();
    if (s_grp_page != NULL)
        lv_group_del(s_grp_page);
    s_grp_page = lv_group_create();
    lv_group_set_wrap(s_grp_page, true);
    lv_group_set_default(s_grp_page);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev != NULL)
        lv_indev_set_group(indev, s_grp_page);
}

static void page_group_exit(void)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev != NULL && s_grp_desktop != NULL)
        lv_indev_set_group(indev, s_grp_desktop);
    if (s_grp_desktop != NULL)
        lv_group_set_default(s_grp_desktop);
    if (s_grp_page != NULL)
    {
        lv_group_del(s_grp_page);
        s_grp_page = NULL;
    }
    for (int i = 0; i < 256; i++)
        if (s_tiles[i] != NULL && lv_obj_is_valid(s_tiles[i]))
        {
            if (s_grp_desktop != NULL)
                lv_group_focus_obj(s_tiles[i]);
            break;
        }
}

static void desktop_open(int idx);
static void build_grid(void);

// ---------------------------------------------------------------- 提示行 / 页码

static void set_hint(const char *s)
{
    if (s_hint != NULL && lv_obj_is_valid(s_hint))
        lv_label_set_text(s_hint, s ? s : "");
}

static void set_page_label(int page)
{
    if (s_page_lbl == NULL || !lv_obj_is_valid(s_page_lbl))
        return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d/%d", page + 1, s_pages_n > 0 ? s_pages_n : 1);
    lv_label_set_text(s_page_lbl, buf);
}

static const char *desktop_hint(void)
{
    return (s_pages_n > 1) ? "滚轮 选择  按下 进入  录像=下页"
                           : "滚轮 选择  按下 进入  相机在第一个方块";
}

// ---------------------------------------------------------------- 进入 / 退出方块

// 焦点组隔离：桌面（主菜单）一个组，每个页面一个独立组。
// 否则页面滚轮焦点会串到背后桌面的方块（滚到底点一下直接进相机）。
static void tile_click_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(t);
    desktop_open(idx);
}

// 占位 / 出错页（插件打不开时也用它，绝不崩）
static lv_obj_t *placeholder_create(lv_obj_t *parent, const char *name, const char *sub)
{
    lv_obj_t *t1 = lv_label_create(parent);
    lv_label_set_text(t1, name);
    lv_obj_set_style_text_font(t1, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t1, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *t2 = lv_label_create(parent);
    lv_obj_set_width(t2, 290);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(t2, sub ? sub : "建设中");
    lv_obj_set_style_text_font(t2, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t2, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(t2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *t3 = lv_label_create(parent);
    lv_label_set_text(t3, "菜单键 返回桌面");
    lv_obj_set_style_text_font(t3, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(t3, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(t3, LV_ALIGN_BOTTOM_MID, 0, -8);
    return t1;
}

// 建一张全屏页面容器（应用页 / 插件页共用）
static lv_obj_t *make_page(void)
{
    lv_obj_t *p = lv_obj_create(lv_scr_act());
    lv_obj_set_size(p, 320, 240);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(p, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(p);
    return p;
}

static void desktop_open(int idx)
{
    const lios_app_def_t *app = lios_apps_get(idx);
    if (app == NULL)
        return;



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

    // ---------------- 插件页（.so） ----------------
    if (lios_apps_is_plugin(idx))
    {
        lios_log("桌面：打开插件应用 %s（%s）", app->id, app->name);
        s_page_hooks = NULL;
        s_page = make_page();
        s_page_idx = idx;
        page_group_enter();
        lv_indev_enable(NULL, false); // 插件自己收滚轮/按键
        s_page_wheel_focus = false;
        s_enc_prev = lios_enc_pressed();
        s_plug_ops = lios_apps_plugin_open(idx);
        if (s_plug_ops == NULL)
        {
            placeholder_create(s_page, app->name, "打不开这个应用，看 /tmp/lios.log");
            set_hint("打不开这个应用  菜单键 返回");
            return;
        }
        s_plug_idx = idx;
        s_plug_ops->create(s_page);
        set_hint("菜单键 返回桌面");
        lv_obj_invalidate(s_page);
        return;
    }

    // ---------------- 内置页面 ----------------
    lios_log("桌面：打开应用 %s", app->id);
    s_page_hooks = NULL;
    s_page_wheel_focus = app->wheel_focus;
    s_page = make_page();
    s_page_idx = idx;
    page_group_enter();

    // 列表类页面（wheel_focus=true）自己用滚轮走焦点；别的页面关掉编码器输入，
    // 免得滚轮/确认去动背后桌面上的方块
    lv_indev_enable(NULL, app->wheel_focus);

    if (app->create != NULL)
        app->create(s_page);
    else
        placeholder_create(s_page, app->name, "建设中");

    set_hint("菜单键 返回桌面");
    lv_obj_invalidate(s_page);
}

// 退出当前页面（UI 线程、已持锁）
static void desktop_close_page(void)
{
    if (s_page == NULL)
        return;
    const lios_app_def_t *app = (s_page_idx >= 0) ? lios_apps_get(s_page_idx) : NULL;
    lios_log("桌面：关闭应用页 %s", app != NULL ? app->id : "?");

    if (s_plug_idx >= 0)
    {
        // 插件：先让它自己清理（删定时器/释放 malloc），再删页面，最后 dlclose
        if (s_plug_ops != NULL && s_plug_ops->destroy != NULL)
            s_plug_ops->destroy();
        if (lv_obj_is_valid(s_page))
            lv_obj_del(s_page);
        s_page = NULL;
        lios_apps_plugin_close(s_plug_idx); // dlclose（必须在删完控件之后）
        s_plug_ops = NULL;
        s_plug_idx = -1;
    }
    else
    {
        if (s_page_hooks != NULL && s_page_hooks->destroy != NULL)
            s_page_hooks->destroy();
        if (lv_obj_is_valid(s_page))
            lv_obj_del(s_page);
        s_page = NULL;
    }

    s_page_hooks = NULL;
    s_page_wheel_focus = false;
    s_page_idx = -1;
    page_group_exit();
    lv_indev_enable(NULL, true); // 恢复滚轮走焦点
    set_hint(desktop_hint());
}

// ---------------------------------------------------------------- 网格（分页）

static void build_grid(void)
{
    if (s_grid != NULL && lv_obj_is_valid(s_grid))
        lv_obj_del(s_grid);
    s_grid = NULL;
    for (int i = 0; i < 24; i++)
        s_pages[i] = NULL;
    for (int i = 0; i < 256; i++)
        s_tiles[i] = NULL;

    s_total = lios_apps_total();
    if (s_total > 256)
        s_total = 256;
    s_pages_n = (s_total + PER_PAGE - 1) / PER_PAGE;
    if (s_pages_n < 1)
        s_pages_n = 1;
    if (s_pages_n > 24)
        s_pages_n = 24;

    lv_obj_t *scr = lv_scr_act();
    s_grid = lv_obj_create(scr);
    lv_obj_set_size(s_grid, 320, PAGE_H);
    lv_obj_set_pos(s_grid, 0, GRID_Y);
    lv_obj_set_style_bg_opa(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // 只允许水平滚动：这就是「页与页水平排列、往右加页」
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);

    int built = 0;
    for (int p = 0; p < s_pages_n; p++)
    {
        lv_obj_t *page = lv_obj_create(s_grid);
        lv_obj_set_size(page, PAGE_W, PAGE_H);
        lv_obj_set_pos(page, p * PAGE_W, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        s_pages[p] = page;

        for (int k = 0; k < PER_PAGE; k++)
        {
            int i = p * PER_PAGE + k;
            if (i >= s_total)
                break;
            const lios_app_def_t *app = lios_apps_get(i);
            if (app == NULL)
                continue;
            int col = k % GRID_COLS;
            int row = k / GRID_COLS;

            lv_obj_t *t = lv_btn_create(page);
            lv_obj_set_size(t, TILE_W, TILE_H);
            lv_obj_set_pos(t, GRID_X + col * (TILE_W + TILE_GAP_X),
                           TILE_Y0 + row * (TILE_H + TILE_GAP_Y));
            lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(t, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_set_style_radius(t, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(t, lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(t, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(t, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

            lv_obj_t *ic = lv_label_create(t);
            lv_label_set_text(ic, app->icon != NULL ? app->icon : "?");
            lv_obj_set_style_text_font(ic, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(ic, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 1);

            lv_obj_t *nm = lv_label_create(t);
            lv_obj_set_width(nm, TILE_W - 8);
            lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(nm, app->name != NULL ? app->name : "?");
            lv_obj_set_style_text_font(nm, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(nm, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -2);

            lv_obj_set_user_data(t, (void *)(intptr_t)i);
            lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED, NULL);
            s_tiles[i] = t;
            built++;
        }
    }

    s_shown_page = 0;
    lv_obj_scroll_to_x(s_grid, 0, LV_ANIM_OFF);
    set_page_label(0);
    if (s_tiles[0] != NULL)
        lv_group_focus_obj(s_tiles[0]);

    lios_log("桌面：网格已建，共 %d 个应用（内置 %d 个 + .so %d 个），分 %d 页",
             s_total, LIOS_APP_COUNT, s_total - LIOS_APP_COUNT, s_pages_n);
}

// 录像键：翻到下一页（循环）
static void goto_page(int page)
{
    if (s_pages_n <= 1)
        return;
    if (page < 0)
        page = s_pages_n - 1;
    if (page >= s_pages_n)
        page = 0;
    int first = page * PER_PAGE;
    if (first >= s_total)
        first = 0;
    // 整页翻页：直接滚到页面边界（不能用 scroll_to_view，那只会把焦点滚进视野，
    // 焦点在右边那列就只滚到右边一半 -> 用户看到的"翻一半"）
    if (s_grid != NULL && lv_obj_is_valid(s_grid))
        lv_obj_scroll_to_x(s_grid, page * PAGE_W, LV_ANIM_ON);
    if (s_tiles[first] != NULL && lv_obj_is_valid(s_tiles[first]))
        lv_group_focus_obj(s_tiles[first]);
    s_shown_page = page;
    set_page_label(page);
}

// 网格滚动时（滚轮跨页）同步页码
static void sync_page_label(void)
{
    if (s_grid == NULL || !lv_obj_is_valid(s_grid) || s_pages_n <= 0)
        return;
    lv_coord_t x = lv_obj_get_scroll_x(s_grid);
    int p = (int)((x + PAGE_W / 2) / PAGE_W);
    if (p < 0)
        p = 0;
    if (p > s_pages_n - 1)
        p = s_pages_n - 1;
    if (p != s_shown_page)
    {
        s_shown_page = p;
        set_page_label(p);
    }
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

    build_grid();

    s_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 4, -3);
    lv_label_set_text(s_hint, desktop_hint());

    s_page_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_page_lbl, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_page_lbl, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_page_lbl, LV_ALIGN_BOTTOM_RIGHT, -4, -3);
    set_page_label(0);
}

// ---------------------------------------------------------------- 插件页按键转发

static void plug_send(int key, int event)
{
    if (s_plug_ops != NULL && s_plug_ops->key != NULL)
        s_plug_ops->key(key, event);
}

static void plugin_forward(bool menu, bool rec)
{
    // 滚轮 -> 左右（每转一格发一次「按下+抬起」；插件里也可以自己看 hold 状态）
    int d = lios_enc_take();
    for (int i = 0; i < 8 && d > 0; i++, d--)
    {
        plug_send(LIOS_KEY_RIGHT, LIOS_EV_PRESS);
        plug_send(LIOS_KEY_RIGHT, LIOS_EV_RELEASE);
    }
    for (int i = 0; i < 8 && d < 0; i++, d++)
    {
        plug_send(LIOS_KEY_LEFT, LIOS_EV_PRESS);
        plug_send(LIOS_KEY_LEFT, LIOS_EV_RELEASE);
    }
    bool down = lios_enc_pressed();
    if (down != s_enc_prev)
    {
        plug_send(LIOS_KEY_ENTER, down ? LIOS_EV_PRESS : LIOS_EV_RELEASE);
        s_enc_prev = down;
    }
    if (rec)
    {
        plug_send(LIOS_KEY_REC, LIOS_EV_PRESS);
        plug_send(LIOS_KEY_REC, LIOS_EV_RELEASE);
    }
    if (menu)
    {
        plug_send(LIOS_KEY_BACK, LIOS_EV_PRESS);
        plug_send(LIOS_KEY_BACK, LIOS_EV_RELEASE);
    }
}

// 页面运行期切换「滚轮给 LVGL 焦点」还是「页面自己收」（查看器要用）
void lios_desktop_set_wheel_focus(bool on)
{
    s_page_wheel_focus = on;
    lv_indev_enable(NULL, on);
}

// ---------------------------------------------------------------- 每帧逻辑

void lios_desktop_tick(void)
{
    // 电源键长按 -> 关机（要碰文件/串口，所以放这里而不是 LVGL 回调里）
    if (lios_power_key_check())
        return;

    // 插件页：所有按键都转发给插件
    if (s_plug_idx >= 0)
    {
        bool menu = lios_key_take(LIOS_K_MENU);
        bool rec = lios_key_take(LIOS_K_REC);
        (void)lios_key_take(LIOS_K_ENTER); // 插件用「按下/抬起」状态，别用布尔
        plugin_forward(menu, rec);
        if (menu)
        {
            lios_log("返回：菜单键 -> 关闭插件页");
            desktop_close_page();
        }
        return;
    }

    // 页面钩子每帧（真页面在这里取 worker 线程的结果、更新界面）
    if (s_page != NULL && s_page_hooks != NULL && s_page_hooks->tick != NULL)
        s_page_hooks->tick();

    // 不用 LVGL 焦点的页面（查看器等）：滚轮由页面自己收
    if (s_page != NULL && !s_page_wheel_focus)
    {
        int d = lios_enc_take();
        if (d != 0 && s_page_hooks != NULL && s_page_hooks->wheel != NULL)
            s_page_hooks->wheel(d);
    }

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

    // ---------------- 桌面上 ----------------
    // 插件清单到货了（worker 扫完）→ 重建网格（每页 9 个，多出来的往右加页）
    int rev = lios_apps_revision();
    if (rev != s_seen_rev)
    {
        s_seen_rev = rev;
        if (s_grid == NULL || lios_apps_total() != s_total)
        {
            build_grid();
            set_hint(desktop_hint());
        }
        else
        {
            // 清单没变化（比如一个插件都没扫到）也要更新一次页码/提示
            set_page_label(s_shown_page);
            set_hint(desktop_hint());
        }
    }

    sync_page_label();

    if (menu)
        set_hint("已经在桌面（相机是第一个方块）");
    else if (rec)
        goto_page(s_shown_page + 1); // 录像键 = 翻到下一页（循环）
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
    set_hint(desktop_hint());
    lios_log("桌面：相机已退出，整屏重绘");
}

bool lios_desktop_page_open(void)
{
    return s_page != NULL;
}
