// ============================================================================
//  俄罗斯方块 —— LiOS 可 dlopen 插件应用（T113 相机 / LVGL 8.4 / 320x240 横屏）
//
//  移植自 ESP32-S3 版 TetrisGame.h（TFT_eSPI + U8g2），逻辑保持一致：
//      7 种方块定义 / 顺时针旋转带简单踢墙 / 消行 / 计分 / 等级加速 / 下一个方块
//
//  本文件相对原版的改动：
//      * 阻塞式 while 主循环  ->  lv_timer 驱动（绝不 sleep / 死循环）
//      * 整屏 sprite 重绘     ->  200 个 lv_obj 小方块，只改颜色，不做逐像素刷新
//      * 引脚按键             ->  lios_app_key() 事件（滚轮 + 3 键）
//      * 定时器一律用 lv_timer，退出时在 lios_app_destroy() 里删干净
//
//  屏幕布局（320x240 横屏，游戏区竖放）：
//      左侧  x=5   宽 110  高 220  y=10   -> 10 列 x 20 行，每格 11x11
//      右侧  x=130 宽 190                -> 下一个 / 分数 / 消行 / 等级 / 操作提示
//
//  按键映射：
//      LIOS_KEY_LEFT  / LIOS_KEY_RIGHT  左右移动（长按连发）
//      LIOS_KEY_DOWN                    软降（长按连发加速下落）
//      LIOS_KEY_ENTER                   旋转（准备 / 结束界面 = 开始 / 重开）
//      LIOS_KEY_REC                     硬降（直接落底并结算）
//      LIOS_KEY_POWER                   暂停 / 继续（额外送的操作，BACK 交给桌面退出）
//      LIOS_KEY_BACK                    不处理，由桌面统一处理退出
// ============================================================================

#include "lios_app.h"
#include "my_main.h" // ui_font_cn16 等字体声明

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// 前置声明：tw_show_over 在定义之前就被调用（编译报 was not declared）
static void tw_show_over(void);

// ---------------------------------------------------------------- 尺寸常量
#define TW_COLS 10        // 游戏区列数
#define TW_ROWS 20        // 游戏区行数
#define TW_CELL 11        // 每格像素（11x11）
#define TW_BOARD_W (TW_COLS * TW_CELL) // 110
#define TW_BOARD_H (TW_ROWS * TW_CELL) // 220
#define TW_BOARD_X 5
#define TW_BOARD_Y 10

#define TW_NEXT_X 140 // 右侧「下一个」预览框
#define TW_NEXT_Y 48
#define TW_NEXT_CELL 11
#define TW_NEXT_W (4 * TW_NEXT_CELL + 8) // 52
#define TW_NEXT_H (4 * TW_NEXT_CELL + 8)

#define TW_PANEL_X 130 // 右侧信息面板
#define TW_PANEL_W 186
#define TW_VALUE_X (TW_PANEL_X + 54)

// ---------------------------------------------------------------- 配色（黑色背景 + 深灰网格）
#define TW_C_BG lv_color_hex(0x000000)    // 整屏背景
#define TW_C_GRID lv_color_hex(0x1A1A1A)  // 空网格格
#define TW_C_EMPTY lv_color_hex(0x101010) // 预览框空格
#define TW_C_BORDER lv_color_hex(0x2E2E2E)
#define TW_C_TEXT lv_color_hex(0xE8E8E8)
#define TW_C_DIM lv_color_hex(0x9AA0A6)
#define TW_C_SCORE lv_color_hex(0xFFD54F)
#define TW_C_LEVEL lv_color_hex(0x4FC3F7)

// ---------------------------------------------------------------- 游戏状态
enum
{
    TW_READY = 0, // 准备开始（等待确认键）
    TW_PLAY,      // 游戏中
    TW_PAUSE,     // 暂停
    TW_OVER       // 游戏结束
};

// 一个方块 = 4x4 掩码 + 颜色
typedef struct
{
    uint8_t m[4][4];
    lv_color_t color;
} tw_shape_t;

// 支持长按连发的 4 个操作轴：左移 / 右移 / 软降 / 旋转
enum
{
    TW_AXIS_LEFT = 0,
    TW_AXIS_RIGHT,
    TW_AXIS_SOFT,
    TW_AXIS_ROT,
    TW_AXIS_NUM
};

// 全局只有一份游戏数据（按任务要求 malloc 一块，destroy 时 free）
typedef struct
{
    uint8_t board[TW_ROWS][TW_COLS]; // 0 = 空，1 = 有块（颜色在数据结构外，重绘时按 1 上色）
    uint8_t lines[TW_ROWS][TW_COLS]; // 每格的方块类型 0..6（+1），用于还原颜色
    tw_shape_t cur;                  // 当前方块
    tw_shape_t next;                 // 下一个方块
    int cur_x;                       // 当前方块左上角列（相对 4x4 网格）
    int cur_y;                       // 当前方块左上角行
    uint8_t cur_type;                // 当前方块类型 0..6
    uint8_t next_type;               // 下一个方块类型

    int state;
    int score;
    int line_count;
    int level;

    uint32_t drop_interval; // 当前等级的下落周期 ms
    uint32_t drop_tick;     // 下落倒计时累计（在 lv_timer 中累加）
    uint32_t elapsed_ms;    // 本局开始时刻（lv_tick_get()，结束时取差值算用时）

    // 长按连发：hold = 是否按住；last_rep = 上次触发时刻（用于判断间隔）
    // 用「绝对时刻差」而不是「每次累加周期」，这样 LVGL 定时器抖动时也不会连跳好几格
    bool hold[TW_AXIS_NUM];
    uint32_t last_rep[TW_AXIS_NUM];

    uint32_t rng; // 简易 xorshift，避免 rand() 的额外开销
} tw_game_t;

static tw_game_t *g = NULL;

static lv_obj_t *g_root = NULL;    // 本应用最外层对象
static lv_obj_t *g_overlay = NULL; // 准备 / 暂停 / 结束遮罩
static lv_obj_t *g_ov_title = NULL;
static lv_obj_t *g_ov_sub = NULL;
static lv_obj_t *g_ov_hint = NULL;
static lv_obj_t *g_cells[TW_ROWS][TW_COLS]; // 游戏区格子
static lv_obj_t *g_prev[4][4];              // 预览格子
static lv_obj_t *g_lb_score = NULL;
static lv_obj_t *g_lb_lines = NULL;
static lv_obj_t *g_lb_level = NULL;

static lv_timer_t *g_timer = NULL; // 下落计时器

// 7 种方块的颜色（对应原版 colors[7]：红 绿 蓝 黄 青 橙 紫）
static const uint32_t kColors[7] = {
    0xFF3B30, // I 红
    0x34C759, // O 绿
    0x3B82F6, // T 蓝
    0xFFD60A, // J 黄
    0x32D0E0, // L 青
    0xFF9F0A, // S 橙
    0xC81CE8  // Z 亮紫
};

static const uint8_t kShapes[7][4][4] = {
    {{0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}}, // 长条
    {{0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, // 方块
    {{0, 1, 0, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, // T
    {{1, 0, 0, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, // J
    {{0, 0, 1, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, // L
    {{0, 1, 1, 0}, {1, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, // S
    {{1, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}  // Z
};

// 长按连发参数（沿用原版手感）
#define TW_KEY_DELAY 180 // 首次连发延迟
#define TW_KEY_RATE 60   // 连发间隔
#define TW_ROT_DELAY 260 // 旋转连发延迟

// ============================================================================
//  小工具
// ============================================================================
static lv_color_t tw_color_of(uint8_t type)
{
    return lv_color_hex(kColors[type % 7]);
}

// 纯黑容器：无边框 / 无圆角 / 不滚动（避免出现滚动条或额外渲染开销）
static void tw_plain_obj(lv_obj_t *o)
{
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void tw_set_bg(lv_obj_t *o, lv_color_t c)
{
    lv_obj_set_style_bg_color(o, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// 统一字体（中文用 ui_font_cn16，数字也用同一套，保证风格一致）
static void tw_font(lv_obj_t *o, const lv_font_t *f)
{
    lv_obj_set_style_text_font(o, f, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_obj_t *tw_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t c, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    tw_font(l, f);
    lv_obj_set_style_text_color(l, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ============================================================================
//  游戏核心（逻辑与 ESP32 原版一一对应）
// ============================================================================
static uint32_t tw_rand(void)
{
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return g->rng;
}

static uint8_t tw_rand_type(void)
{
    return (uint8_t)(tw_rand() % 7);
}

// 碰撞检测：把当前方块按 (ox, oy) 偏移后检测是否越界或压到已有块
static bool tw_collision(int ox, int oy)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
        {
            if (!g->cur.m[y][x])
                continue;
            int nx = g->cur_x + x + ox;
            int ny = g->cur_y + y + oy;
            if (nx < 0 || nx >= TW_COLS || ny >= TW_ROWS)
                return true;
            if (ny >= 0 && g->board[ny][nx])
                return true;
        }
    return false;
}

// 刷新整块游戏区：200 个格子只改颜色，不销毁重建对象
static void tw_sync_board(void)
{
    if (!g_root)
        return;

    for (int y = 0; y < TW_ROWS; y++)
    {
        for (int x = 0; x < TW_COLS; x++)
        {
            lv_obj_t *o = g_cells[y][x];
            if (!o)
                continue;

            int type = -1; // -1 = 空
            if (g->board[y][x])
                type = (int)g->lines[y][x] - 1;
            else
            {
                // 当前方块覆盖到的格子（悬浮在棋盘之上临时上色）
                int bx = x - g->cur_x;
                int by = y - g->cur_y;
                if (bx >= 0 && bx < 4 && by >= 0 && by < 4 && g->cur.m[by][bx])
                    type = (int)g->cur_type;
            }

            tw_set_bg(o, type < 0 ? TW_C_GRID : tw_color_of((uint8_t)type));
        }
    }
}

// 刷新右侧「下一个」预览
static void tw_sync_preview(void)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            if (!g_prev[r][c])
                continue;
            if (g->next.m[r][c])
                tw_set_bg(g_prev[r][c], g->next.color);
            else
                tw_set_bg(g_prev[r][c], TW_C_EMPTY);
        }
}

static void tw_sync_stats(void)
{
    if (g_lb_score)
        lv_label_set_text_fmt(g_lb_score, "%d", g->score);
    if (g_lb_lines)
        lv_label_set_text_fmt(g_lb_lines, "%d", g->line_count);
    if (g_lb_level)
        lv_label_set_text_fmt(g_lb_level, "%d", g->level);
}

// 按等级更新下落周期
static void tw_update_speed(void)
{
    uint32_t p = 500; // 1 级 500ms（对应原版 dropInterval = 500）
    if (g->level > 1)
        p = 500 - (uint32_t)(g->level - 1) * 45;
    if (p < 80)
        p = 80; // 最快 80ms，避免高等级瞬间到底
    g->drop_interval = p;

    if (g_timer)
        lv_timer_set_period(g_timer, (uint32_t)g->drop_interval);
}

// 生成下一个方块
static void tw_pick_next(void)
{
    g->next_type = tw_rand_type();
    memcpy(g->next.m, kShapes[g->next_type], sizeof(g->next.m));
    g->next.color = tw_color_of(g->next_type);
    tw_sync_preview();
}

// 取出「下一个」作为当前方块，并生成新的「下一个」
static void tw_new_shape(void)
{
    g->cur_x = 3;
    g->cur_y = 0;
    g->cur_type = g->next_type;
    g->cur = g->next;
    tw_pick_next();

    g->drop_tick = 0;
    if (tw_collision(0, 0))
    {
        // 出生点就被占住 = 游戏结束
        g->state = TW_OVER;
    }
}

// 把当前方块固定进棋盘
static void tw_lock(void)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (g->cur.m[y][x])
            {
                int nx = g->cur_x + x;
                int ny = g->cur_y + y;
                if (nx >= 0 && nx < TW_COLS && ny >= 0 && ny < TW_ROWS)
                {
                    g->board[ny][nx] = 1;
                    g->lines[ny][nx] = (uint8_t)(g->cur_type + 1);
                }
            }
}

// 消行：满行下移一行，计分 10 分/行，每消 5 行升一级
static void tw_clear_lines(void)
{
    int cleared = 0;
    for (int y = TW_ROWS - 1; y >= 0; y--)
    {
        bool full = true;
        for (int x = 0; x < TW_COLS; x++)
            if (!g->board[y][x])
            {
                full = false;
                break;
            }

        if (full)
        {
            for (int ty = y; ty > 0; ty--)
            {
                memcpy(g->board[ty], g->board[ty - 1], TW_COLS);
                memcpy(g->lines[ty], g->lines[ty - 1], TW_COLS);
            }
            memset(g->board[0], 0, TW_COLS);
            memset(g->lines[0], 0, TW_COLS);
            cleared++;
            y++; // 同一行位置需要重新检查（上面的行落下来了）
        }
    }

    if (cleared > 0)
    {
        g->score += cleared * 10;
        g->line_count += cleared;

        int lv = g->line_count / 5 + 1;
        if (lv > 20)
            lv = 20;
        if (lv != g->level)
        {
            g->level = lv;
            tw_update_speed();
        }
        tw_sync_stats();
    }
}

// 固定 + 消行 + 出新块
static void tw_lock_and_next(void)
{
    tw_lock();
    tw_clear_lines();
    tw_new_shape();
    g->drop_tick = 0;
}

// 旋转（顺时针），带简单踢墙：先原位，再左右各试 1、2 格
static void tw_rotate(void)
{
    if (g->state != TW_PLAY)
        return;

    uint8_t ns[4][4] = {{0}};
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            ns[x][3 - y] = g->cur.m[y][x];

    uint8_t old[4][4];
    memcpy(old, g->cur.m, sizeof(old));
    memcpy(g->cur.m, ns, sizeof(ns));

    const int kicks[5] = {0, -1, 1, -2, 2};
    for (int i = 0; i < 5; i++)
    {
        if (!tw_collision(kicks[i], 0))
        {
            g->cur_x += kicks[i];
            tw_sync_board();
            return;
        }
    }

    memcpy(g->cur.m, old, sizeof(old)); // 五个位置都不行 -> 放弃这次旋转
}

// 软降：能下就下，不能下就立刻结算
static void tw_soft_drop(void)
{
    if (g->state != TW_PLAY)
        return;

    if (!tw_collision(0, 1))
    {
        g->cur_y++;
        g->drop_tick = 0;
        tw_sync_board();
    }
    else
    {
        tw_lock_and_next();
        tw_sync_board();
        if (g->state == TW_OVER)
            tw_show_over();
    }
}

// 硬降：一路落到底再结算
static void tw_hard_drop(void)
{
    if (g->state != TW_PLAY)
        return;

    while (!tw_collision(0, 1))
        g->cur_y++;

    tw_lock_and_next();
    tw_sync_board();
    if (g->state == TW_OVER)
        tw_show_over();
}

static void tw_move(int dx)
{
    if (g->state != TW_PLAY)
        return;
    if (tw_collision(dx, 0))
        return;

    g->cur_x += dx;
    g->drop_tick = 0; // 与原版一致：移动后重新计时
    tw_sync_board();
}

// ---------------------------------------------------------------- 遮罩层（准备 / 暂停 / 结束）
static void tw_overlay_show(const char *title, lv_color_t tcolor, const char *sub, const char *hint)
{
    if (!g_overlay)
        return;

    lv_label_set_text(g_ov_title, title);
    lv_obj_set_style_text_color(g_ov_title, tcolor, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_ov_sub, sub ? sub : "");
    lv_label_set_text(g_ov_hint, hint ? hint : "");

    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_overlay); // 保证压在所有格子上方
}

static void tw_overlay_hide(void)
{
    if (g_overlay)
        lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void tw_show_ready(void)
{
    // 注意：ui_font_cn16 里没有「俄/罗/斯/游/戏」这些字（字体是子集化的），
    // 直接写会渲染成空方块。这里只用字体里确实有的字，别改回原标题。
    tw_overlay_show("方块", TW_C_LEVEL, "左边是方块区", "确认键开始");
}

static void tw_show_over(void)
{
    char sub[64];
    // elapsed_ms 存的是「开局时刻」，这里取差值；用无符号相减可安全跨越 tick 回绕
    uint32_t sec = (uint32_t)(lv_tick_get() - g->elapsed_ms) / 1000;
    snprintf(sub, sizeof(sub), "分数 %d  消行 %d  用时 %02u:%02u",
             g->score, g->line_count, (unsigned)(sec / 60), (unsigned)(sec % 60));
    tw_overlay_show("结束", lv_color_hex(0xFF5252), sub, "确认键重开");
}

// ---------------------------------------------------------------- 开局 / 暂停
static void tw_reset_board(void)
{
    memset(g->board, 0, sizeof(g->board));
    memset(g->lines, 0, sizeof(g->lines));
}

static void tw_game_start(void)
{
    tw_reset_board();

    g->score = 0;
    g->line_count = 0;
    g->level = 1;
    g->drop_tick = 0;
    g->elapsed_ms = lv_tick_get();
    memset(g->hold, 0, sizeof(g->hold));
    memset(g->last_rep, 0, sizeof(g->last_rep));
    g->state = TW_PLAY;

    tw_update_speed();

    tw_pick_next(); // 先备好「下一个」
    tw_new_shape(); // 再取出来当当前方块

    tw_sync_board();
    tw_sync_stats();
    tw_overlay_hide();
}

static void tw_toggle_pause(void)
{
    if (g->state == TW_PLAY)
    {
        g->state = TW_PAUSE;
        memset(g->hold, 0, sizeof(g->hold)); // 暂停时清掉长按状态，避免恢复后连跳
        tw_overlay_show("暂停", TW_C_TEXT, "已暂停", "电源键恢复");
    }
    else if (g->state == TW_PAUSE)
    {
        g->state = TW_PLAY;
        g->drop_tick = 0;
        tw_overlay_hide();
    }
}

// ---------------------------------------------------------------- 下落心跳（每 20ms 走一次）
//  一个 lv_timer_t 同时负责：重力下落 + 长按连发，避免多开定时器
//  所有计时都用 lv_tick_get() 算绝对差值，不依赖回调一定准时
static void tw_repeat_scan(uint32_t now);

static void tw_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g)
        return;

    uint32_t now = lv_tick_get();

    // 只在游戏进行中推进时间 / 下落 / 连发
    if (g->state != TW_PLAY)
        return;

    g->elapsed_ms = now;

    // 长按连发（左右 / 软降 / 旋转）
    tw_repeat_scan(now);

    // 连发里可能直接触发游戏结束
    if (g->state != TW_PLAY)
        return;

    // 重力：按等级周期自动下落
    g->drop_tick += 20; // 回调周期固定 20ms
    if (g->drop_tick < g->drop_interval)
        return;
    g->drop_tick = 0;

    if (!tw_collision(0, 1))
    {
        g->cur_y++;
        tw_sync_board();
    }
    else
    {
        tw_lock_and_next();
        tw_sync_board();
        if (g->state == TW_OVER)
            tw_show_over();
    }
}

// 长按连发（左 / 右 / 旋转）：在同一个心跳里检查
// last_rep[i] 存的是「下一次允许触发的时刻」，用无符号差值判断，天然防抖也不会补跳
static void tw_repeat_scan(uint32_t now)
{
    const int key_of[TW_AXIS_NUM] = {LIOS_KEY_LEFT, LIOS_KEY_RIGHT, LIOS_KEY_DOWN, LIOS_KEY_ENTER};

    for (int i = 0; i < TW_AXIS_NUM; i++)
    {
        if (!g->hold[i])
            continue;
        if (g->state != TW_PLAY) // 已被暂停 / 结束
            return;
        if ((uint32_t)(now - g->last_rep[i]) < TW_KEY_DELAY)
            continue;

        uint32_t next_gap = TW_KEY_RATE; // 首次之后的连发间隔

        switch (key_of[i])
        {
        case LIOS_KEY_LEFT:
            tw_move(-1);
            break;
        case LIOS_KEY_RIGHT:
            tw_move(1);
            break;
        case LIOS_KEY_ENTER:
            tw_rotate();
            next_gap = TW_ROT_DELAY; // 旋转连发慢一些，避免误触连转两下
            break;
        case LIOS_KEY_DOWN:
            tw_soft_drop(); // 软降：按住就一路加速往下
            break;
        default:
            break;
        }

        g->last_rep[i] = now + next_gap;
    }
}

// ============================================================================
//  界面搭建
// ============================================================================
static void tw_build_board(lv_obj_t *root)
{
    lv_obj_t *box = lv_obj_create(root);
    tw_plain_obj(box);
    lv_obj_set_pos(box, TW_BOARD_X, TW_BOARD_Y);
    lv_obj_set_size(box, TW_BOARD_W, TW_BOARD_H);
    tw_set_bg(box, TW_C_BG);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(box, TW_C_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (int y = 0; y < TW_ROWS; y++)
        for (int x = 0; x < TW_COLS; x++)
        {
            lv_obj_t *cell = lv_obj_create(box);
            if (!cell) // 内存吃紧时降级：这一格不画，但游戏逻辑照常跑，也不会崩
                continue;
            tw_plain_obj(cell);
            // 格子画 10x10 并留 1px 间隙，透出黑色底板就成了「深灰细线」网格
            lv_obj_set_size(cell, TW_CELL - 1, TW_CELL - 1);
            lv_obj_set_pos(cell, x * TW_CELL + 1, y * TW_CELL + 1);
            tw_set_bg(cell, TW_C_GRID);
            g_cells[y][x] = cell;
        }

    // 遮罩层：尺寸与游戏区一致，盖在格子上方
    g_overlay = lv_obj_create(root);
    tw_plain_obj(g_overlay);
    lv_obj_set_pos(g_overlay, TW_BOARD_X, TW_BOARD_Y);
    lv_obj_set_size(g_overlay, TW_BOARD_W, TW_BOARD_H);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_overlay, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(g_overlay, TW_C_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_overlay, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(g_overlay, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_ov_title = lv_label_create(g_overlay);
    tw_font(g_ov_title, &ui_font_cn16);
    lv_obj_set_style_text_color(g_ov_title, TW_C_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_ov_sub = lv_label_create(g_overlay);
    tw_font(g_ov_sub, &ui_font_cn16);
    lv_obj_set_style_text_color(g_ov_sub, TW_C_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(g_ov_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_ov_sub, TW_BOARD_W - 16);
    lv_obj_set_style_text_align(g_ov_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_ov_hint = lv_label_create(g_overlay);
    tw_font(g_ov_hint, &ui_font_cn16);
    lv_obj_set_style_text_color(g_ov_hint, TW_C_DIM, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void tw_build_preview(lv_obj_t *root)
{
    tw_label(root, "下一个", &ui_font_cn16, TW_C_DIM, TW_NEXT_X, 12);

    lv_obj_t *box = lv_obj_create(root);
    tw_plain_obj(box);
    lv_obj_set_pos(box, TW_NEXT_X, TW_NEXT_Y);
    lv_obj_set_size(box, TW_NEXT_W, TW_NEXT_H);
    tw_set_bg(box, TW_C_BG);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(box, TW_C_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            lv_obj_t *cell = lv_obj_create(box);
            if (!cell) // 内存不足时跳过（也不会崩）
                continue;
            tw_plain_obj(cell);
            lv_obj_set_size(cell, TW_NEXT_CELL, TW_NEXT_CELL);
            lv_obj_set_pos(cell, 4 + c * TW_NEXT_CELL, 4 + r * TW_NEXT_CELL);
            tw_set_bg(cell, TW_C_EMPTY);
            g_prev[r][c] = cell;
        }
}

static void tw_build_panel(lv_obj_t *root)
{
    // 分数 / 消行 / 等级（标签 + 数值两列）
    tw_label(root, "分数", &ui_font_cn16, TW_C_DIM, TW_PANEL_X, 104);
    g_lb_score = tw_label(root, "0", &ui_font_cn16, TW_C_SCORE, TW_VALUE_X, 104);

    tw_label(root, "消行", &ui_font_cn16, TW_C_DIM, TW_PANEL_X, 128);
    g_lb_lines = tw_label(root, "0", &ui_font_cn16, TW_C_TEXT, TW_VALUE_X, 128);

    tw_label(root, "关", &ui_font_cn16, TW_C_DIM, TW_PANEL_X, 152);
    g_lb_level = tw_label(root, "1", &ui_font_cn16, TW_C_LEVEL, TW_VALUE_X, 152);

    // 操作提示（相机只有滚轮 + 3 个键）
    tw_label(root, "左右:移动", &ui_font_cn16, TW_C_DIM, TW_PANEL_X, 180);
    tw_label(root, "下:落 确认:变", &ui_font_cn16, TW_C_DIM, TW_PANEL_X, 198);
    tw_label(root, "录像:落底 电源:暂停", &ui_font_cn16, TW_C_DIM, TW_PANEL_X, 216);

    (void)root;
}

// ============================================================================
//  插件导出接口
// ============================================================================
extern "C" const lios_app_info_t lios_app_info = {
    "tetris",         // id
    "方块",         // 桌面显示名（字体里没有俄/罗/斯）
    "方",             // 图标
    30,               // 排序
    1                 // 需要独占整屏
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    // 保险：万一桌面没调 destroy 就再进一次
    if (g)
    {
        free(g);
        g = NULL;
    }

    g = (tw_game_t *)calloc(1, sizeof(tw_game_t));
    if (!g)
        return NULL;

    // 简易随机种子（不用 rand()，避免依赖 libc 的实现与额外开销）
    g->rng = (uint32_t)time(NULL) ^ 0x9E3779B9u;
    if (g->rng == 0)
        g->rng = 0x12345678u;

    memset(g_cells, 0, sizeof(g_cells));
    memset(g_prev, 0, sizeof(g_prev));
    g_overlay = g_ov_title = g_ov_sub = g_ov_hint = NULL;
    g_lb_score = g_lb_lines = g_lb_level = NULL;

    // 最外层容器：铺满 320x240，纯黑，不滚动
    g_root = lv_obj_create(parent);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, 320, 240);
    lv_obj_set_style_bg_color(g_root, TW_C_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    tw_font(g_root, &ui_font_cn16);

    tw_build_board(g_root);
    tw_build_preview(g_root);
    tw_build_panel(g_root);

    g->level = 1;
    g->state = TW_READY;

    tw_sync_board();
    tw_sync_preview();
    tw_sync_stats();
    tw_show_ready();

    // 下落心跳：固定 20ms 走一次，靠 drop_interval 控制实际下落速度
    g_timer = lv_timer_create(tw_timer_cb, 20, NULL);

    return g_root;
}

extern "C" void lios_app_destroy(void)
{
    // 先停心跳，避免 LVGL 之后还回调到已经释放的数据
    if (g_timer)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }

    // LVGL 对象由桌面容器统一删除，这里只把本地指针清空（防止变成野指针）
    g_root = NULL;
    g_overlay = g_ov_title = g_ov_sub = g_ov_hint = NULL;
    g_lb_score = g_lb_lines = g_lb_level = NULL;
    memset(g_cells, 0, sizeof(g_cells));
    memset(g_prev, 0, sizeof(g_prev));

    // 释放自己 malloc 的游戏数据
    if (g)
    {
        free(g);
        g = NULL;
    }
}

// 需要长按连发的按键 -> 操作轴下标（-1 = 不参与连发）
static int tw_axis_of(int key)
{
    switch (key)
    {
    case LIOS_KEY_LEFT:
        return TW_AXIS_LEFT;
    case LIOS_KEY_RIGHT:
        return TW_AXIS_RIGHT;
    case LIOS_KEY_DOWN:
        return TW_AXIS_SOFT;
    case LIOS_KEY_ENTER:
        return TW_AXIS_ROT;
    default:
        return -1;
    }
}

extern "C" void lios_app_key(int key, int event)
{
    if (!g)
        return;

    int axis = tw_axis_of(key);

    // 抬起：结束长按连发（必须处理，否则按住状态会一直保留）
    if (event == LIOS_EV_RELEASE)
    {
        if (axis >= 0)
            g->hold[axis] = false;
        return;
    }

    // 只处理「按下」，长按（LIOS_EV_LONG）不重复触发（连发交给心跳定时器）
    if (event != LIOS_EV_PRESS)
        return;

    uint32_t now = lv_tick_get();

    // 准备 / 结束界面：确认键开始新的一局（与屏幕提示「确认键开始」保持一致）
    if (g->state == TW_READY || g->state == TW_OVER)
    {
        if (key == LIOS_KEY_ENTER)
            tw_game_start();
        return;
    }

    // 暂停中：只响应电源键恢复，其余按键忽略
    if (g->state == TW_PAUSE)
    {
        if (key == LIOS_KEY_POWER)
            tw_toggle_pause();
        return;
    }

    // 先记下按住状态：首次延迟从「按下时刻」开始算
    if (axis >= 0)
    {
        g->hold[axis] = true;
        g->last_rep[axis] = now;
    }

    switch (key)
    {
    case LIOS_KEY_LEFT:
        tw_move(-1);
        break;
    case LIOS_KEY_RIGHT:
        tw_move(1);
        break;
    case LIOS_KEY_DOWN:
        tw_soft_drop(); // 软降：能下就下一格
        break;
    case LIOS_KEY_ENTER:
        tw_rotate(); // 旋转
        break;
    case LIOS_KEY_REC:
        tw_hard_drop(); // 硬降：直接落底并结算
        break;
    case LIOS_KEY_POWER:
        tw_toggle_pause(); // 额外的暂停键
        break;
    case LIOS_KEY_BACK:
    default:
        // BACK 由桌面统一处理退出，这里不拦截
        break;
    }
}
