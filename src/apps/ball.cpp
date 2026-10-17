// ============================================================================
//  弹球游戏 —— 忠实移植自用户自己的 ESP32-S3 项目 ESP-S3/BallGame.h（608 行，已逐行读完）
//
//  【原版对照表】（行号 = BallGame.h）
//   L14-23   240x240；挡板 40x8；PADDLE_Y = H-8-2；球 8px；背景 (12,14,24)
//            -> 本移植 320x240 横屏；挡板 52（40×320/240≈53）；球仍 8px；PADDLE_Y=230；同背景色
//   L26-28   难度 4 档 {"简单","中等","困难","超难"} = {0.8,1.0,1.5,2.2}，默认第 2 档
//   L97-121  resetGame：球回中心；baseSpeed=2.0×系数（本移植再乘屏宽比 1.333 保持手感）；
//            X 随机方向、Y=-baseSpeed；分数清零
//   L124-135 updatePaddle：diff=target-pos；accel=diff*0.3；vel=vel*0.7+accel；|vel|>10 夹紧；
//            pos+=vel；夹边界；paddleX=(int)(pos+0.5)                  —— 逐行照抄
//   L139-147 左右键把 paddleTargetX ∓5（本移植：滚轮每格 ∓12，因为滚轮是离散的）
//   L151-183 确认键短按=暂停/继续；长按≥1500ms=退出
//   L202-255 updateGame：位移→左右上墙反弹→接球（仅 canScore 时）hitPos=(球心-挡板心)；
//            speedX=constrain(hitPos/5,-6,6)；speedY=-|speedY|；score++；200ms 后 canScore 恢复；
//            |speedX|<7 时 += ±0.12×难度系数；ballY>=H → GAME_OVER（**没有生命，掉一次就结束**）
//   L258-328 HUD 左"得分 N"中"时间 m:ss.d"右"帧率 N"；暂停=全屏变暗+"暂停中"+"难度: x"
//   L331-449 结束动画：3 圈扩散（1.5s，缓动 1-(1-p)^3）→"游戏结束"+三行统计（得分/时间/难度）
//            +"确认:重玩 任意键:退出"；确认=重玩；无操作 1.5s 自动退出
//   L452-603 难度菜单：标题"弹球游戏/Ball Game"、4 张卡片（含描述）、高亮条缓动滑动
//            （hlY += (target-hlY)*0.22）、底部按键提示
//
//  本平台按键（桌面转发规则见 lios/desktop.cpp 的 plugin_forward）：
//    滚轮       -> LIOS_KEY_LEFT / LIOS_KEY_RIGHT（每格一对 按下+抬起）
//    波轮按下   -> LIOS_KEY_ENTER（按下/抬起，长按要自己按时间判断）
//    录像键     -> LIOS_KEY_REC
//    菜单键     -> LIOS_KEY_BACK（**桌面会同时关闭本页 = 退出应用**）
//  因此：菜单键 = 退出应用；录像键 = 回到难度菜单（原版"任意键退出"的位置）
//
//  纪律：桌面转发的按键与 lv_timer 回调都在 UI 线程 → 只用普通变量，不加锁、不做阻塞 I/O
// ============================================================================

#include "lios_app.h"
// lios_log 由主程序 lios 导出（-rdynamic），插件可直接调用（写 /tmp/lios.log）
extern "C" void lios_log(const char *fmt, ...);
#include "my_main.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 尺寸（对应原版 L14-23）
#define G_W 320
#define G_H 240
#define PADDLE_W 52
#define PADDLE_H 8
#define PADDLE_Y (G_H - PADDLE_H - 2)
#define BALL 8
#define SPEED_SCALE 1.333f
#define FRAME_MS 16 // 原版 TARGET_FRAME_TIME = 16

// ---------------------------------------------------------------- 难度（原版 L26-28 / L468-472）
static const char *kDiffNames[4] = {"简单", "中等", "困难", "超难"};
static const char *kDiffDescs[4] = {"入门速度 轻松上手", "标准速度 经典体验",
                                    "高速挑战 反应考验", "极限速度 终极试炼"};
static const float kDiffFactors[4] = {0.8f, 1.0f, 1.5f, 2.2f};
static lv_color_t g_diffColor[4];

// ---------------------------------------------------------------- 状态机
enum
{
    ST_MENU = 0,
    ST_PLAY,
    ST_PAUSE,
    ST_OVER_ANIM,
    ST_OVER
};

// ---- 对象
static lv_obj_t *g_root = NULL;
static lv_obj_t *g_hud_score = NULL, *g_hud_time = NULL, *g_hud_fps = NULL;
static lv_obj_t *g_paddle = NULL, *g_paddle_hl = NULL;
static lv_obj_t *g_ball = NULL, *g_ball_dot = NULL, *g_trail[2] = {NULL, NULL};
static lv_obj_t *g_menu[4] = {0}, *g_menu_txt[4] = {0}, *g_menu_desc[4] = {0};
static lv_obj_t *g_menu_title = NULL, *g_menu_sub = NULL, *g_menu_hint = NULL, *g_hl = NULL;
static lv_obj_t *g_pause_box = NULL, *g_pause_t1 = NULL, *g_pause_t2 = NULL;
static lv_obj_t *g_over_title = NULL, *g_over_box[3] = {0}, *g_over_lbl[3] = {0},
                *g_over_val[3] = {0}, *g_over_hint = NULL;
static lv_obj_t *g_circle[3] = {NULL, NULL, NULL};
static lv_timer_t *g_timer = NULL;

// ---- 游戏变量（原版 L31-79）
static int g_state = ST_MENU;
static int g_diff = 1;
static int g_paddle_x = (G_W - PADDLE_W) / 2;
static float g_paddle_pos = (G_W - PADDLE_W) / 2.0f;
static float g_paddle_target = (G_W - PADDLE_W) / 2.0f;
static float g_paddle_vel = 0.0f;
static float g_ball_x = G_W / 2.0f, g_ball_y = G_H / 2.0f;
static float g_ball_sx = 2.0f, g_ball_sy = -2.0f;
static int g_score = 0;
static bool g_can_score = true;
static uint32_t g_last_score_ms = 0;
static uint32_t g_start_ms = 0;
static float g_game_time = 0.0f;
static uint32_t g_over_ms = 0, g_anim_ms = 0;
static float g_anim_cx = 0, g_anim_cy = 0;
static uint32_t g_frames = 0, g_fps_t0 = 0;
static float g_fps = 0.0f;
static float g_hl_y = 0.0f;
static bool g_pause_blink = true;
static uint32_t g_blink_ms = 0;

// ---- 输入（桌面转发，UI 线程）
static int g_left_cnt = 0, g_right_cnt = 0, g_rec_cnt = 0;
static bool g_enter_down = false;
static uint32_t g_enter_ms = 0;
static bool g_enter_long = false;
static int g_enter_clicks = 0;

static void menu_show(bool on);
static void play_show(bool on);
static void over_show(bool on);
static void pause_show(bool on);
static void start_game(void);
static void go_menu(void);

// ---------------------------------------------------------------- 小工具
static lv_obj_t *mk_rect(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int r)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, r, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    return o;
}

static lv_obj_t *mk_circle(lv_obj_t *p, int dia, lv_color_t c)
{
    lv_obj_t *o = mk_rect(p, 0, 0, dia, dia, c, LV_RADIUS_CIRCLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(l, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    return l;
}

// ---------------------------------------------------------------- 建界面
static void build_all(lv_obj_t *parent)
{
    g_root = parent;
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x0C0E18), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_diffColor[0] = lv_color_hex(0x288C50);
    g_diffColor[1] = lv_color_hex(0x2864B4);
    g_diffColor[2] = lv_color_hex(0xC87828);
    g_diffColor[3] = lv_color_hex(0xC83232);

    // HUD（原版 L283-297）
    g_hud_score = mk_label(g_root, "得分 0", &ui_font_cn12, lv_color_hex(0x64D28C));
    lv_obj_set_pos(g_hud_score, 6, 4);
    g_hud_time = mk_label(g_root, "0:00.0", &ui_font_cn12, lv_color_hex(0xB4B9C3));
    lv_obj_align(g_hud_time, LV_ALIGN_TOP_MID, 0, 4);
    g_hud_fps = mk_label(g_root, "帧率 --", &ui_font_cn12, lv_color_hex(0x787D8C));
    lv_obj_align(g_hud_fps, LV_ALIGN_TOP_RIGHT, -6, 4);

    // 挡板（原版 L263-265）
    g_paddle = mk_rect(g_root, g_paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, lv_color_hex(0x1EB4DC), 4);
    g_paddle_hl = mk_rect(g_root, g_paddle_x + 4, PADDLE_Y + 2, PADDLE_W - 8, 2,
                          lv_color_hex(0xB4F0FF), 3);

    // 球 + 拖尾 + 高光（原版 L268-276）
    g_trail[0] = mk_circle(g_root, BALL - 2, lv_color_hex(0x28503C));
    g_trail[1] = mk_circle(g_root, BALL - 4, lv_color_hex(0x14283C));
    g_ball = mk_circle(g_root, BALL, lv_color_white());
    g_ball_dot = mk_circle(g_root, 2, lv_color_hex(0xC8E6FF));

    // 暂停遮罩（原版 L300-325）
    g_pause_box = mk_rect(g_root, 0, 0, G_W, G_H, lv_color_hex(0x080810), 0);
    lv_obj_set_style_bg_opa(g_pause_box, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
    g_pause_t1 = mk_label(g_root, "暂停中", &ui_font_cn16, lv_color_hex(0xFFC83C));
    lv_obj_align(g_pause_t1, LV_ALIGN_CENTER, 0, -18);
    g_pause_t2 = mk_label(g_root, "难度: 中等", &ui_font_cn12, lv_color_hex(0xA0A5AF));
    lv_obj_align(g_pause_t2, LV_ALIGN_CENTER, 0, 18);

    // 结束：3 个扩散圆圈（原版 L331-362）
    for (int i = 0; i < 3; i++)
    {
        static const uint32_t cols[3] = {0xFF3C3C, 0xC88C3C, 0x3C8CDC};
        g_circle[i] = mk_circle(g_root, 4, lv_color_hex(cols[i]));
        lv_obj_set_style_bg_opa(g_circle[i], LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(g_circle[i], 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(g_circle[i], lv_color_hex(cols[i]), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    // 结束画面（原版 L364-404）
    g_over_title = mk_label(g_root, "游戏结束", &ui_font_cn16, lv_color_hex(0xFF3C3C));
    lv_obj_align(g_over_title, LV_ALIGN_TOP_MID, 0, 26);
    static const char *labels[3] = {"得分", "时间", "难度"};
    for (int i = 0; i < 3; i++)
    {
        g_over_box[i] = mk_rect(g_root, 40, 70 + i * 38, 240, 30, lv_color_hex(0x14161F), 6);
        lv_obj_set_style_border_width(g_over_box[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(g_over_box[i], lv_color_hex(0x282C37), LV_PART_MAIN | LV_STATE_DEFAULT);
        g_over_lbl[i] = mk_label(g_over_box[i], labels[i], &ui_font_cn12, lv_color_hex(0x8C919B));
        lv_obj_align(g_over_lbl[i], LV_ALIGN_LEFT_MID, 10, 0);
        g_over_val[i] = mk_label(g_over_box[i], "-", &ui_font_cn12, lv_color_white());
        lv_obj_align(g_over_val[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }
    g_over_hint = mk_label(g_root, "确认:重玩   录像:回菜单", &ui_font_cn12, lv_color_hex(0x646973));
    lv_obj_align(g_over_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    // 难度菜单（原版 L486-543）
    g_menu_title = mk_label(g_root, "弹球游戏", &ui_font_cn16, lv_color_hex(0x64C8FF));
    lv_obj_align(g_menu_title, LV_ALIGN_TOP_MID, 0, 8);
    g_menu_sub = mk_label(g_root, "Ball Game", &ui_font_cn12, lv_color_hex(0x50A0C8));
    lv_obj_align(g_menu_sub, LV_ALIGN_TOP_MID, 0, 30);
    const int cardW = 250, cardH = 34, cardX = (G_W - cardW) / 2, baseY = 52, gap = 38;
    for (int i = 0; i < 4; i++)
    {
        int cy = baseY + i * gap;
        g_menu[i] = mk_rect(g_root, cardX, cy, cardW, cardH, lv_color_hex(0x0E1016), 8);
        lv_obj_set_style_border_width(g_menu[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(g_menu[i], lv_color_hex(0x23262D), LV_PART_MAIN | LV_STATE_DEFAULT);
        mk_rect(g_menu[i], 2, 4, 4, cardH - 8, g_diffColor[i], 2); // 左侧色条（原版 L509）
        g_menu_txt[i] = mk_label(g_menu[i], kDiffNames[i], &ui_font_cn16, lv_color_hex(0xA0A5AF));
        lv_obj_align(g_menu_txt[i], LV_ALIGN_LEFT_MID, 14, 0);
        g_menu_desc[i] = mk_label(g_menu[i], kDiffDescs[i], &ui_font_cn12, lv_color_hex(0x5A5F69));
        lv_obj_align(g_menu_desc[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }
    g_hl = mk_rect(g_root, cardX, baseY + g_diff * gap, cardW, cardH, lv_color_hex(0x121622), 8);
    lv_obj_set_style_bg_opa(g_hl, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_hl, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(g_hl, g_diffColor[g_diff], LV_PART_MAIN | LV_STATE_DEFAULT);
    g_menu_hint = mk_label(g_root, "滚轮 选择   确认 开始   菜单 退出", &ui_font_cn12,
                           lv_color_hex(0x464B55));
    lv_obj_align(g_menu_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    g_hl_y = (float)(baseY + g_diff * gap);
    menu_show(true);
    play_show(false);
    over_show(false);
    pause_show(false);
}

// ---------------------------------------------------------------- 显隐
static void set_vis(lv_obj_t *o, bool on)
{
    if (o == NULL || !lv_obj_is_valid(o))
        return;
    if (on)
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void menu_show(bool on)
{
    lv_obj_t *o[] = {g_menu_title, g_menu_sub, g_hl, g_menu_hint, g_menu[0], g_menu[1], g_menu[2], g_menu[3]};
    for (unsigned i = 0; i < sizeof(o) / sizeof(o[0]); i++)
        set_vis(o[i], on);
}

static void play_show(bool on)
{
    lv_obj_t *o[] = {g_paddle, g_paddle_hl, g_ball, g_ball_dot, g_trail[0], g_trail[1],
                     g_hud_score, g_hud_time, g_hud_fps};
    for (unsigned i = 0; i < sizeof(o) / sizeof(o[0]); i++)
        set_vis(o[i], on);
}

static void over_show(bool on)
{
    lv_obj_t *o[] = {g_over_title, g_over_hint, g_over_box[0], g_over_box[1], g_over_box[2],
                     g_circle[0], g_circle[1], g_circle[2]};
    for (unsigned i = 0; i < sizeof(o) / sizeof(o[0]); i++)
        set_vis(o[i], on);
}

static void pause_show(bool on)
{
    set_vis(g_pause_box, on);
    set_vis(g_pause_t1, on);
    set_vis(g_pause_t2, on);
    if (on)
    {
        lv_obj_move_foreground(g_pause_box);
        lv_obj_move_foreground(g_pause_t1);
        lv_obj_move_foreground(g_pause_t2);
    }
}

// ---------------------------------------------------------------- 逻辑（照抄原版）
static void reset_game(void) // 原版 L97-121
{
    g_paddle_x = (G_W - PADDLE_W) / 2;
    g_paddle_pos = (float)g_paddle_x;
    g_paddle_target = (float)g_paddle_x;
    g_paddle_vel = 0.0f;
    g_ball_x = G_W / 2.0f;
    g_ball_y = G_H / 2.0f;
    float base = 2.0f * kDiffFactors[g_diff] * SPEED_SCALE;
    g_ball_sx = (rand() & 1) ? base : -base; // 原版 random(0,2)
    g_ball_sy = -base;
    g_score = 0;
    g_can_score = true;
    g_start_ms = lv_tick_get();
    g_game_time = 0.0f;
    g_frames = 0;
    g_fps_t0 = g_start_ms;
    g_pause_blink = true;
    g_blink_ms = g_start_ms;
    g_state = ST_PLAY;
}

static void start_game(void)
{
    reset_game();
    menu_show(false);
    over_show(false);
    pause_show(false);
    play_show(true);
}

static void go_menu(void)
{
    g_state = ST_MENU;
    play_show(false);
    over_show(false);
    pause_show(false);
    menu_show(true);
    g_hl_y = (float)(52 + g_diff * 38);
}

static void update_paddle(void) // 原版 L124-135
{
    float diff = g_paddle_target - g_paddle_pos;
    float accel = diff * 0.3f;
    g_paddle_vel = g_paddle_vel * 0.7f + accel;
    if (fabsf(g_paddle_vel) > 10.0f)
        g_paddle_vel = (g_paddle_vel > 0) ? 10.0f : -10.0f;
    g_paddle_pos += g_paddle_vel;
    if (g_paddle_pos < 0)
        g_paddle_pos = 0;
    if (g_paddle_pos > G_W - PADDLE_W)
        g_paddle_pos = (float)(G_W - PADDLE_W);
    g_paddle_x = (int)(g_paddle_pos + 0.5f);
}

static void update_game(void) // 原版 L202-255
{
    if (g_state != ST_PLAY)
        return;

    update_paddle();

    g_ball_x += g_ball_sx;
    g_ball_y += g_ball_sy;

    if (g_ball_x <= 0)
    {
        g_ball_x = 0;
        g_ball_sx = fabsf(g_ball_sx);
    }
    if (g_ball_x + BALL >= G_W)
    {
        g_ball_x = (float)(G_W - BALL);
        g_ball_sx = -fabsf(g_ball_sx);
    }
    if (g_ball_y <= 0)
    {
        g_ball_y = 0;
        g_ball_sy = fabsf(g_ball_sy);
    }

    int ptop = PADDLE_Y, pbot = PADDLE_Y + PADDLE_H;
    if (g_ball_y + BALL >= ptop && g_ball_y + BALL <= pbot && g_ball_x + BALL >= g_paddle_x &&
        g_ball_x <= g_paddle_x + PADDLE_W && g_can_score)
    {
        g_ball_y = (float)(ptop - BALL);
        int hitPos = (int)((g_ball_x + BALL / 2) - (g_paddle_x + PADDLE_W / 2));
        int sx = hitPos / 5;
        if (sx > 6)
            sx = 6;
        if (sx < -6)
            sx = -6;
        g_ball_sx = (float)sx;
        g_ball_sy = -fabsf(g_ball_sy);
        g_score++;
        g_can_score = false;
        g_last_score_ms = lv_tick_get();
        float inc = 0.12f * kDiffFactors[g_diff];
        if (fabsf(g_ball_sx) < 7.0f)
            g_ball_sx += (g_ball_sx > 0) ? inc : -inc;
    }

    if (g_ball_y >= G_H) // 掉球 = 结束（没有生命）
    {
        g_state = ST_OVER_ANIM;
        g_anim_ms = lv_tick_get();
        g_anim_cx = g_ball_x + BALL / 2.0f;
        g_anim_cy = g_ball_y + BALL / 2.0f;
        for (int i = 0; i < 3; i++)
            lv_obj_set_size(g_circle[i], 4, 4);
        play_show(false);
        over_show(true);
        set_vis(g_over_title, false);
        for (int i = 0; i < 3; i++)
            set_vis(g_over_box[i], false);
        set_vis(g_over_hint, false);
        return;
    }

    if (!g_can_score && (lv_tick_get() - g_last_score_ms > 200))
        g_can_score = true;
}

static void update_over(void) // 原版 L331-449
{
    if (g_state == ST_OVER_ANIM)
    {
        uint32_t el = lv_tick_get() - g_anim_ms;
        float p = el / 1500.0f;
        if (p > 1.0f)
            p = 1.0f;
        float eased = 1.0f - powf(1.0f - p, 3.0f);
        float dx = (g_anim_cx > G_W - g_anim_cx) ? g_anim_cx : (G_W - g_anim_cx);
        float dy = (g_anim_cy > G_H - g_anim_cy) ? g_anim_cy : (G_H - g_anim_cy);
        float maxR = sqrtf(dx * dx + dy * dy);
        float f[3] = {eased, (p - 0.2f) / 0.8f, (p - 0.4f) / 0.6f};
        for (int i = 0; i < 3; i++)
        {
            float v = f[i] < 0 ? 0 : (f[i] > 1 ? 1 : f[i]);
            int dia = (int)(v * maxR * 2);
            if (dia < 4)
                dia = 4;
            lv_obj_set_size(g_circle[i], dia, dia);
            lv_obj_set_pos(g_circle[i], (int)g_anim_cx - dia / 2, (int)g_anim_cy - dia / 2);
        }
        if (p >= 1.0f)
        {
            g_state = ST_OVER;
            g_over_ms = lv_tick_get();
            for (int i = 0; i < 3; i++)
                set_vis(g_circle[i], false);
            set_vis(g_over_title, true);
            for (int i = 0; i < 3; i++)
                set_vis(g_over_box[i], true);
            set_vis(g_over_hint, true);
            char b[32];
            snprintf(b, sizeof(b), "%d", g_score);
            lv_label_set_text(g_over_val[0], b);
            int m = (int)g_game_time / 60, s = (int)g_game_time % 60;
            int t = (int)((g_game_time - (int)g_game_time) * 10);
            snprintf(b, sizeof(b), "%d:%02d.%d", m, s, t);
            lv_label_set_text(g_over_val[1], b);
            lv_label_set_text(g_over_val[2], kDiffNames[g_diff]);
        }
    }
    else if (g_state == ST_OVER)
    {
        if (lv_tick_get() - g_over_ms >= 1500) // 原版 L442-444：无操作自动退出
            go_menu();
    }
}

static void update_draw(void) // 原版 L258-297
{
    if (g_state != ST_PLAY && g_state != ST_PAUSE)
        return;

    lv_obj_set_pos(g_paddle, g_paddle_x, PADDLE_Y);
    lv_obj_set_pos(g_paddle_hl, g_paddle_x + 4, PADDLE_Y + 2);

    int bcx = (int)g_ball_x + BALL / 2, bcy = (int)g_ball_y + BALL / 2;
    for (int t = 2; t >= 1; t--) // 拖尾
    {
        int tx = bcx - (int)(g_ball_sx * t * 2);
        int ty = bcy - (int)(g_ball_sy * t * 2);
        int dia = BALL - t * 2;
        if (dia < 2)
            dia = 2;
        lv_obj_set_size(g_trail[t - 1], dia, dia);
        lv_obj_set_pos(g_trail[t - 1], tx - dia / 2, ty - dia / 2);
    }
    lv_obj_set_pos(g_ball, (int)g_ball_x, (int)g_ball_y);
    lv_obj_set_pos(g_ball_dot, (int)g_ball_x + 1, (int)g_ball_y + 1);

    char b[48];
    snprintf(b, sizeof(b), "得分 %d", g_score);
    lv_label_set_text(g_hud_score, b);
    if (g_state != ST_OVER)
    {
        int m = (int)g_game_time / 60, s = (int)g_game_time % 60;
        int t = (int)((g_game_time - (int)g_game_time) * 10);
        snprintf(b, sizeof(b), "%d:%02d.%d", m, s, t);
        lv_label_set_text(g_hud_time, b);
    }
    snprintf(b, sizeof(b), "帧率 %.0f", g_fps);
    lv_label_set_text(g_hud_fps, b);
}

// ---------------------------------------------------------------- 定时器
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();

    g_frames++;
    if (now - g_fps_t0 >= 1000)
    {
        g_fps = (float)g_frames;
        g_frames = 0;
        g_fps_t0 = now;
    }

    // 长按确认（≥1500ms）= 回难度菜单（原版长按=退出游戏）
    if (g_enter_down && !g_enter_long && (now - g_enter_ms >= 1500))
    {
        g_enter_long = true;
        go_menu();
    }

    // ---- 菜单 ----
    if (g_state == ST_MENU)
    {
        while (g_left_cnt > 0)
        {
            g_left_cnt--;
            g_diff = (g_diff - 1 + 4) % 4;
        }
        while (g_right_cnt > 0)
        {
            g_right_cnt--;
            g_diff = (g_diff + 1) % 4;
        }
        float target = (float)(52 + g_diff * 38); // 高亮缓动（原版 L484）
        g_hl_y += (target - g_hl_y) * 0.22f;
        lv_obj_set_y(g_hl, (int)g_hl_y);
        lv_obj_set_style_border_color(g_hl, g_diffColor[g_diff], LV_PART_MAIN | LV_STATE_DEFAULT);
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_color(g_menu_txt[i], (i == g_diff) ? lv_color_white()
                                                                     : lv_color_hex(0xA0A5AF),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_move_foreground(g_hl);
        while (g_enter_clicks > 0)
        {
            g_enter_clicks--;
            start_game();
            return;
        }
        if (g_rec_cnt > 0)
            g_rec_cnt = 0;
        return;
    }

    // ---- 对局 / 暂停 ----
    if (g_state == ST_PLAY || g_state == ST_PAUSE)
    {
        while (g_left_cnt > 0) // 滚轮移挡板（原版每帧 ∓5，这里每格 ∓12）
        {
            g_left_cnt--;
            g_paddle_target -= 12.0f;
            if (g_paddle_target < 0)
                g_paddle_target = 0;
        }
        while (g_right_cnt > 0)
        {
            g_right_cnt--;
            g_paddle_target += 12.0f;
            if (g_paddle_target > G_W - PADDLE_W)
                g_paddle_target = (float)(G_W - PADDLE_W);
        }
        while (g_enter_clicks > 0) // 短按 = 暂停/继续（原版 L165-179）
        {
            g_enter_clicks--;
            if (g_state == ST_PLAY)
            {
                g_state = ST_PAUSE;
                pause_show(true);
                char b[48];
                snprintf(b, sizeof(b), "难度: %s", kDiffNames[g_diff]);
                lv_label_set_text(g_pause_t2, b);
            }
            else
            {
                g_state = ST_PLAY;
                pause_show(false);
            }
        }
        if (g_rec_cnt > 0)
        {
            g_rec_cnt = 0;
            go_menu();
            return;
        }

        static uint32_t last = 0;
        if (last == 0)
            last = now;
        if (g_state == ST_PLAY)
            g_game_time += (now - last) / 1000.0f;
        last = now;

        update_game();
        update_draw();

        // 暂停时闪烁（原版 L302-307）
        if (g_state == ST_PAUSE)
        {
            if (now - g_blink_ms >= 400)
            {
                g_blink_ms = now;
                g_pause_blink = !g_pause_blink;
                set_vis(g_pause_t1, g_pause_blink);
            }
        }
        return;
    }

    // ---- 结束 ----
    if (g_state == ST_OVER_ANIM || g_state == ST_OVER)
    {
        while (g_enter_clicks > 0)
        {
            g_enter_clicks--;
            start_game(); // 确认 = 重玩
            return;
        }
        if (g_rec_cnt > 0)
        {
            g_rec_cnt = 0;
            go_menu();
            return;
        }
        update_over();
    }
}

// ---------------------------------------------------------------- ABI
extern "C" const lios_app_info_t lios_app_info = {
    "ball",       // id
    "弹球游戏",   // 名称（字体已有这些字）
    "球",         // 图标
    20,           // 顺序
    1             // 独占整屏
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_left_cnt = g_right_cnt = g_rec_cnt = 0;
    g_enter_down = false;
    g_enter_long = false;
    g_enter_clicks = 0;
    g_state = ST_MENU;
    g_fps = 0.0f;
    g_frames = 0;
    g_fps_t0 = lv_tick_get();
    srand((unsigned)lv_tick_get());

    build_all(parent);
    if (g_timer == NULL)
        g_timer = lv_timer_create(tick_cb, FRAME_MS, NULL);
    lios_log("弹球游戏：已启动（原始玩法：纯对打，无砖块无生命）");
    return g_root;
}

extern "C" void lios_app_destroy(void)
{
    if (g_timer != NULL)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
    // LVGL 对象随桌面容器一起删；这里只清指针
    g_root = NULL;
    g_hud_score = g_hud_time = g_hud_fps = NULL;
    g_paddle = g_paddle_hl = NULL;
    g_ball = g_ball_dot = NULL;
    g_trail[0] = g_trail[1] = NULL;
    for (int i = 0; i < 4; i++)
        g_menu[i] = g_menu_txt[i] = g_menu_desc[i] = NULL;
    g_menu_title = g_menu_sub = g_menu_hint = g_hl = NULL;
    g_pause_box = g_pause_t1 = g_pause_t2 = NULL;
    g_over_title = g_over_hint = NULL;
    for (int i = 0; i < 3; i++)
    {
        g_over_box[i] = g_over_lbl[i] = g_over_val[i] = NULL;
        g_circle[i] = NULL;
    }
    lios_log("弹球游戏：已退出并清理");
}

extern "C" void lios_app_key(int key, int event)
{
    if (event == LIOS_EV_RELEASE)
    {
        if (key == LIOS_KEY_ENTER)
        {
            if (g_enter_down)
            {
                g_enter_down = false;
                if (!g_enter_long)
                    g_enter_clicks++; // 短按才算一次点击
            }
        }
        return;
    }
    // PRESS / LONG
    switch (key)
    {
    case LIOS_KEY_LEFT:
        if (g_left_cnt < 16)
            g_left_cnt++;
        break;
    case LIOS_KEY_RIGHT:
        if (g_right_cnt < 16)
            g_right_cnt++;
        break;
    case LIOS_KEY_REC:
        if (g_rec_cnt < 4)
            g_rec_cnt++;
        break;
    case LIOS_KEY_ENTER:
        if (!g_enter_down)
        {
            g_enter_down = true;
            g_enter_ms = lv_tick_get();
            g_enter_long = false;
        }
        break;
    default:
        break; // 菜单键由桌面负责关页面
    }
}
