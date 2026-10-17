// ============================================================================
//  弹球游戏（打砖块）—— LiOS 可 dlopen 插件应用
//  目标平台：全志 T113 相机 / LVGL 8.4 / 320x240 横屏 / musl / 117MB RAM
//
//  移植来源：ESP32-S3 版 BallGame.h（TFT_eSPI + U8g2_for_TFT_eSPI），逻辑一一对应：
//      * 挡板平滑跟随：目标位置 + 加速度/阻尼滤波（paddleTarget / paddlePos / paddleVelocity）
//      * 球碰左右墙 + 顶部反弹、挡板把球弹回、接球位置决定水平反弹角度（hitPos 越大越斜）
//      * 接一次球 +1 分、每次接球略微提速、球落底判负、暂停/继续、结束页
//
//  环境差异导致的改动（逐条记录，方便回看原版）：
//      1) 原版 240x240 竖屏 + sprite 阻塞主循环  ->  320x240 横屏 + lv_timer 固定步长驱动。
//         物理按 16ms 一个子步（BG_STEP_MS）推进，和原版「一帧」完全等价；
//         回调里绝不 sleep、绝不 while 死等。
//      2) 引脚 digitalRead 长按  ->  lios_app_key() 事件（滚轮左右 + 确认键）。
//      3) 原版难度菜单（简单/中等/困难/超难）：滚轮已经用来移动挡板，而且这四个词在
//         ui_font_cn16 里几乎没有字形  ->  固定「中等」基准速度（原版 diffFactors[1] = 1.0），
//         改用关卡 Lv 递增速度（1.0 -> 2.2，正好覆盖原版 0.8~2.2 的难度区间）。
//      4) 原版 canScore + 200ms 冷却防重复计分  ->  改成「球必须由上往下撞挡板，撞到后
//         吸附到挡板上沿」，同样不会重复计分，而且不依赖 millis()。
//      5) 原版没有砖块（纯对打）  ->  加了一面 4x8 砖块墙，让「生命 / Lv / 胜负」有实际意义；
//         打光整面墙 = 过关（Lv+1、砖块重铺、球速上限提高）。
//      6) 缺字保护：ui_font_cn16 只带 0x20-0x7f + 192 个常用汉字，实测没有
//         「得 / 分 / 暂 / 停 / 弹 / 球 / 游 / 戏 / 滚 / 轮」等字，直接写会渲染成占位方块。
//         所以启动时探一次字形（bg_t），缺字就退化成字体里有的同义说法：
//             「得分」->「计数」  「暂停」->「不动」  「游戏结束」->「结束」  「弹球游戏」->「PONG」
//         以后把字体换成更全的版本，会自动用回原词，不用改这里。
//
//  屏幕布局（320x240 横屏）：
//      y  0 ~  21   顶部状态行：得分 xxx  生命 x  Lv.x
//      y 22 ~ 239   游戏区（纯黑）：4 行 x 8 列砖块 + 球（带拖尾）+ 挡板
//
//  按键映射（桌面通过 lios_app_key 转发）：
//      LIOS_KEY_LEFT  / LIOS_KEY_RIGHT  滚轮左右转：挡板左右移动（每格 12px，带平滑加速）
//      LIOS_KEY_ENTER                   开始 / 暂停 / 继续 / 重开（等待发球时 = 立刻发球）
//      LIOS_KEY_POWER                   暂停 / 继续（与确认键等价的备用键）
//      LIOS_KEY_UP    / LIOS_KEY_DOWN   兜底：个别固件把滚轮上报成上下，也当左右用
//      LIOS_KEY_BACK                    不处理，由桌面统一退出
//
//  内存：全部游戏数据在 lios_app_create() 里 calloc 一小块（约 400 字节），
//        lios_app_destroy() 里 free；LVGL 对象由桌面容器统一删除，这里只删自己的定时器。
//
//  线程：lv_timer 回调 = LVGL 线程。lios_app_key() 可能来自 HAL 线程，
//        所以它只做两件不碰控件的事：改挡板目标位置（几个 float）、置一个确认标志；
//        所有会动 lv_obj 的动作都在 bg_timer_cb 里执行。
// ============================================================================

#include "lios_app.h"
#include "my_main.h" // ui_font_cn16 等字体声明

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ---------------------------------------------------------------- 尺寸常量
#define BG_SCREEN_W 320
#define BG_SCREEN_H 240
#define BG_HUD_H 22                              // 顶部状态行高度
#define BG_FIELD_Y BG_HUD_H                      // 游戏区上沿
#define BG_FIELD_H (BG_SCREEN_H - BG_HUD_H)      // 游戏区高度 218

#define BG_BALL 8                                // 球（正方形对象 + 整圆角）
#define BG_PADDLE_W 56                           // 挡板（原版 40，横屏放宽一点更好接球）
#define BG_PADDLE_H 8
#define BG_PADDLE_Y (BG_SCREEN_H - BG_PADDLE_H - 4)

#define BG_BRICK_ROWS 4                          // 砖块墙：4 行 x 8 列 = 32 块
#define BG_BRICK_COLS 8
#define BG_BRICK_W 34
#define BG_BRICK_H 13
#define BG_BRICK_X0 10                           // 10 + 7*38 + 34 = 310，左右各留 10px
#define BG_BRICK_Y0 32                           // 状态行下方 10px
#define BG_BRICK_PITCH_X 38
#define BG_BRICK_PITCH_Y 16

// ---------------------------------------------------------------- 手感 / 时序常量
#define BG_STEP_MS 16                            // 物理固定步长 = 原版一帧
#define BG_TIMER_MS 30                           // lv_timer 周期（约 33fps，够顺滑又不费 CPU）
#define BG_MAX_STEPS 4                           // 单次回调最多补几步，防止卡顿雪崩
#define BG_KEY_STEP 12                           // 滚轮每转一格，挡板目标位移
#define BG_LIVES 3                               // 初始生命
#define BG_SERVE_MS 700                          // 丢命/过关后等待自动发球
#define BG_FLASH_MS 900                          // 过关时状态行提示时长

#define BG_BASE_SPEED 2.0f                       // 原版中等难度：2.0 像素/帧（16ms）
#define BG_SPEED_MAX 2.2f                        // 原版超难难度系数上限
#define BG_BALL_SPEED_MAX 4.0f                   // 球速硬上限（防止高关卡失控）
#define BG_VX_MAX 3.2f                           // 水平速度上限（保证还有足够的垂直分量）

// ---------------------------------------------------------------- 配色（黑色背景）
#define BG_C_FIELD lv_color_hex(0x000000)        // 游戏区底色：纯黑
#define BG_C_HUD lv_color_hex(0x0A0D14)          // 状态行底色
#define BG_C_LINE lv_color_hex(0x1E2635)         // 状态行下沿分隔线
#define BG_C_TEXT lv_color_hex(0xE8EEF4)         // 状态行正文
#define BG_C_GOLD lv_color_hex(0xFFD54F)         // 过关提示色
#define BG_C_DIM lv_color_hex(0x9AA7B4)          // 次要文字
#define BG_C_PADDLE lv_color_hex(0x1EB4DC)       // 挡板主体（原版 30,180,220）
#define BG_C_PADDLE_HI lv_color_hex(0xB4F0FF)    // 挡板高光（原版 180,240,255）
#define BG_C_BALL lv_color_hex(0xFFFFFF)         // 球：纯白
#define BG_C_TRAIL0 lv_color_hex(0x2C5570)       // 拖尾（近端）
#define BG_C_TRAIL1 lv_color_hex(0x16334A)       // 拖尾（远端）
#define BG_C_OVERLAY lv_color_hex(0x000000)      // 遮罩底色

// 砖块墙每行的颜色（自上而下）
static const uint32_t kBgBrickColors[BG_BRICK_ROWS] = {
    0xFF4D4F, // 红
    0xFFA940, // 橙
    0x36CFC9, // 青
    0x8A63F0  // 紫
};

// ---------------------------------------------------------------- 游戏状态
enum
{
    BG_ST_READY = 0, // 准备开始（等确认键）
    BG_ST_PLAY,      // 球在飞
    BG_ST_SERVE,     // 球贴在挡板上，倒计时后自动发球（丢命重生 / 过关后）
    BG_ST_PAUSE,     // 暂停
    BG_ST_OVER       // 游戏结束
};

// 可见性缓存（只在状态真的变化时才 add/clear HIDDEN，避免每帧白白重绘）
#define BG_VIS_UNKNOWN 0
#define BG_VIS_ON 1
#define BG_VIS_OFF 2

// 全局只有一份游戏数据：按任务要求 malloc 一小块，destroy 时 free
typedef struct
{
    int state;                   // BG_ST_*
    int score;                   // 得分（砖块 +10、接球 +1、过关 +50）
    int lives;                   // 剩余生命
    int level;                   // 关卡（打光一面墙 +1）
    float speed_mul;             // 关卡速度系数（替代原版 diffFactors）
    float ball_speed;            // 当前球速（像素/16ms 帧），随接球次数缓慢增加

    float paddle_pos;            // 挡板真实位置（原版 paddlePosX）
    float paddle_target;         // 挡板目标位置（原版 paddleTargetX）
    float paddle_vel;            // 挡板速度（原版 paddleVelocity）
    int paddle_x;                // 挡板整数位置（原版 paddleX）

    float ball_x;                // 球左上角（原版 ballX / ballY）
    float ball_y;
    float ball_vx;               // 原版 ballSpeedX / ballSpeedY
    float ball_vy;

    uint8_t bricks[BG_BRICK_ROWS][BG_BRICK_COLS];        // 1 = 还在，0 = 已打掉
    lv_obj_t *brick_obj[BG_BRICK_ROWS][BG_BRICK_COLS];   // 每块砖的 lv_obj
    int bricks_left;                                     // 还剩几块砖

    int serve_ms;                // 发球倒计时
    int level_flash_ms;          // 过关提示倒计时
    uint32_t acc_ms;             // 固定步长累加器
    uint32_t last_tick;          // 上一次回调的 lv_tick
    uint32_t total_ms;           // 本局累计时长
    uint32_t rng;                // 简易 xorshift（不用 rand()）

    char hud_shown[48];          // 状态行当前文本（相同就不重设，省一次重绘）
    uint8_t hud_flash_on;        // 状态行是否处于过关提示态
    uint8_t vis_paddle;          // 可见性缓存（每帧只做比较，不重复 add/clear）
    uint8_t vis_ball;
    uint8_t vis_trail[2];        // 两段拖尾各自一份缓存
} bg_game_t;

static bg_game_t *g = NULL;

static lv_obj_t *g_root = NULL;    // 本应用最外层对象
static lv_obj_t *g_lb_hud = NULL;  // 状态行文本
static lv_obj_t *g_paddle = NULL;  // 挡板
static lv_obj_t *g_ball = NULL;    // 球
static lv_obj_t *g_trail[2] = {NULL, NULL}; // 拖尾两段

static lv_obj_t *g_overlay = NULL; // 准备 / 暂停 / 结束遮罩
static lv_obj_t *g_ov_title = NULL;
static lv_obj_t *g_ov_sub = NULL;
static lv_obj_t *g_ov_hint = NULL;

static lv_timer_t *g_timer = NULL; // 游戏心跳

// 待处理的「确认键」命令：lios_app_key() 有可能来自 HAL 线程，
// 而所有会动 lv_obj 的动作必须留在 LVGL 线程（lv_timer）里做，
// 所以这里只置一个标志，真正的状态切换在 bg_timer_cb 里执行。
static volatile int g_pending_cmd = 0;

// 字体里存在的说法（下面是兜底值，create() 里会用字形探测的结果覆盖）
static const char *g_word_title = "PONG";
static const char *g_word_score = "计数";
static const char *g_word_life = "生命";
static const char *g_word_pause = "不动";
static const char *g_word_over = "结束";

// ---------------------------------------------------------------- 前向声明
static void bg_hud_update(void);
static void bg_show_overlay(const char *title, const char *sub, const char *hint);
static void bg_hide_overlay(void);
static void bg_new_game(void);
static void bg_launch(void);

// ============================================================================
//  小工具
// ============================================================================
// 纯色方块：无边框 / 无圆角 / 不滚动 / 无内边距（之后按需单独加圆角和边框）
static void bg_plain(lv_obj_t *o)
{
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void bg_set_bg(lv_obj_t *o, lv_color_t c)
{
    lv_obj_set_style_bg_color(o, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// 只在可见性真的变化时才动 HIDDEN 标志（clear_flag 会无条件重绘一次）
static void bg_set_visible(lv_obj_t *o, uint8_t *cache, bool visible)
{
    if (!o)
        return;
    uint8_t want = visible ? BG_VIS_ON : BG_VIS_OFF;
    if (*cache == want)
        return;
    *cache = want;
    if (visible)
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static uint32_t bg_rand(void)
{
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return g->rng;
}

static float bg_clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

// ============================================================================
//  字体缺字保护：ui_font_cn16 只带 0x20-0x7f + 192 个常用汉字，
//  缺字会渲染成占位方块。这里把 UTF-8 拆成码点逐字探一次字形。
// ============================================================================
static bool bg_text_ok(const char *utf8)
{
    if (!utf8)
        return false;

    const unsigned char *p = (const unsigned char *)utf8;
    while (*p)
    {
        uint32_t cp = 0;
        if (*p < 0x80) // ASCII（0x20-0x7f 都在字体里）
        {
            cp = *p;
            p += 1;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            cp = ((uint32_t)(*p & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            p += 2;
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            cp = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                 (uint32_t)(p[2] & 0x3F);
            p += 3;
        }
        else
        {
            cp = ((uint32_t)(*p & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            p += 4;
        }

        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(&ui_font_cn16, &dsc, cp, 0) || dsc.is_placeholder)
            return false;
    }
    return true;
}

// 字体里有 preferred 就用 preferred，否则退回 alt
static const char *bg_t(const char *preferred, const char *alt)
{
    return bg_text_ok(preferred) ? preferred : alt;
}

// ============================================================================
//  游戏逻辑（与 ESP32 原版 BallGame.h 一一对应）
// ============================================================================

// 铺满砖块墙（过关时也用它重铺）
static void bg_reset_bricks(void)
{
    for (int r = 0; r < BG_BRICK_ROWS; r++)
    {
        for (int c = 0; c < BG_BRICK_COLS; c++)
        {
            g->bricks[r][c] = 1;
            if (g->brick_obj[r][c])
                lv_obj_clear_flag(g->brick_obj[r][c], LV_OBJ_FLAG_HIDDEN);
        }
    }
    g->bricks_left = BG_BRICK_ROWS * BG_BRICK_COLS;
}

// 挡板回中（原版 resetGame() 里的 paddleX）
static void bg_paddle_center(void)
{
    g->paddle_pos = (float)((BG_SCREEN_W - BG_PADDLE_W) / 2);
    g->paddle_target = g->paddle_pos;
    g->paddle_vel = 0.0f;
    g->paddle_x = (int)(g->paddle_pos + 0.5f);
}

// 球贴在挡板正上方（丢命重生 / 过关 / 准备开始时都用它）
static void bg_place_ball_on_paddle(void)
{
    g->ball_x = g->paddle_pos + (float)((BG_PADDLE_W - BG_BALL) / 2);
    g->ball_y = (float)(BG_PADDLE_Y - BG_BALL);
    g->ball_vx = 0.0f;
    g->ball_vy = 0.0f;
}

// 进入「等待发球」状态：球跟着挡板走，倒计时结束后自动发射
static void bg_serve_start(void)
{
    g->state = BG_ST_SERVE;
    g->serve_ms = BG_SERVE_MS;
    bg_place_ball_on_paddle();
}

// 发射（原版 resetGame()：随机左右 45 度斜射，速度 = 2.0 * 难度系数）
static void bg_launch(void)
{
    float sp = BG_BASE_SPEED * g->speed_mul;
    if (sp > BG_BALL_SPEED_MAX)
        sp = BG_BALL_SPEED_MAX;

    g->ball_speed = sp;
    g->ball_vx = (bg_rand() & 1u) ? sp : -sp;
    g->ball_vy = -sp;
    g->state = BG_ST_PLAY;
}

// 过关：Lv+1、球速系数提高、砖块重铺（速度区间对应原版 diffFactors 0.8~2.2）
static void bg_level_up(void)
{
    if (g->level < 99)
        g->level++;

    g->speed_mul = 1.0f + (float)(g->level - 1) * 0.25f;
    if (g->speed_mul > BG_SPEED_MAX)
        g->speed_mul = BG_SPEED_MAX;

    g->score += 50; // 过关奖励
    g->level_flash_ms = BG_FLASH_MS;
    bg_reset_bricks();
    bg_serve_start();
}

// 丢命（原版是直接 GAME OVER，这里给了 3 条命）
static void bg_lose_life(void)
{
    g->lives--;
    if (g->lives <= 0)
    {
        g->lives = 0;
        g->state = BG_ST_OVER;
        g->paddle_vel = 0.0f;

        char sub[64];
        uint32_t sec = g->total_ms / 1000;
        // 「用时」两个字都在 ui_font_cn16 里，直接用
        snprintf(sub, sizeof(sub), "%s %d    Lv.%d    用时 %d:%02d",
                 g_word_score, g->score, g->level, (int)(sec / 60), (int)(sec % 60));
        bg_show_overlay(g_word_over, sub, "确认:重开");
    }
    else
    {
        bg_serve_start();
    }
    bg_hud_update();
}

// 砖块碰撞：用「上一步的球位置」判断是从哪个面撞进来的，返回是否命中
static bool bg_hit_brick(float prev_x, float prev_y)
{
    for (int r = 0; r < BG_BRICK_ROWS; r++)
    {
        for (int c = 0; c < BG_BRICK_COLS; c++)
        {
            if (!g->bricks[r][c])
                continue;

            float bx = (float)(BG_BRICK_X0 + c * BG_BRICK_PITCH_X);
            float by = (float)(BG_BRICK_Y0 + r * BG_BRICK_PITCH_Y);

            // 矩形不相交 -> 换下一块
            if (g->ball_x + BG_BALL <= bx || g->ball_x >= bx + BG_BRICK_W)
                continue;
            if (g->ball_y + BG_BALL <= by || g->ball_y >= by + BG_BRICK_H)
                continue;

            // 相交了：看上一帧球完全在砖块的哪一侧，决定反弹轴（角上优先算垂直反弹）
            bool from_vertical = (prev_y + BG_BALL <= by) || (prev_y >= by + BG_BRICK_H);
            bool from_horizontal = (prev_x + BG_BALL <= bx) || (prev_x >= bx + BG_BRICK_W);
            if (from_horizontal && !from_vertical)
            {
                // 从左右撞进来：把球推到砖块外侧，避免下一步还嵌在里面重复判定
                g->ball_x = (g->ball_vx > 0.0f) ? (bx - (float)BG_BALL) : (bx + (float)BG_BRICK_W);
                g->ball_vx = -g->ball_vx;
            }
            else
            {
                g->ball_y = (g->ball_vy > 0.0f) ? (by - (float)BG_BALL) : (by + (float)BG_BRICK_H);
                g->ball_vy = -g->ball_vy;
            }

            // 打掉砖块（隐藏对象，不销毁，方便下一关重铺）
            g->bricks[r][c] = 0;
            g->bricks_left--;
            g->score += 10;
            if (g->brick_obj[r][c])
                lv_obj_add_flag(g->brick_obj[r][c], LV_OBJ_FLAG_HIDDEN);

            if (g->bricks_left <= 0)
                bg_level_up();

            return true; // 一个物理步最多打掉一块（16ms 走 2~4 像素，不可能同时穿两块）
        }
    }
    return false;
}

// 一个固定步（16ms，= 原版一帧）的物理更新
static void bg_step(void)
{
    // ---- 1) 挡板平滑移动：原版 updatePaddle() 的滤波系数原样照搬 ----
    float diff = g->paddle_target - g->paddle_pos;
    float accel = diff * 0.3f;
    g->paddle_vel = g->paddle_vel * 0.7f + accel;
    if (g->paddle_vel > 10.0f)
        g->paddle_vel = 10.0f;
    if (g->paddle_vel < -10.0f)
        g->paddle_vel = -10.0f;
    g->paddle_pos += g->paddle_vel;

    if (g->paddle_pos < 0.0f)
        g->paddle_pos = 0.0f;
    if (g->paddle_pos > (float)(BG_SCREEN_W - BG_PADDLE_W))
        g->paddle_pos = (float)(BG_SCREEN_W - BG_PADDLE_W);

    // 到位就吸附：原版会一直在目标附近微抖，白白触发重绘，这里收一下
    if (fabsf(diff) < 0.5f && fabsf(g->paddle_vel) < 0.5f)
    {
        g->paddle_pos = g->paddle_target;
        g->paddle_vel = 0.0f;
    }
    g->paddle_x = (int)(g->paddle_pos + 0.5f);

    // ---- 2) 准备 / 等待发球：球贴在挡板上，跟着挡板走 ----
    if (g->state == BG_ST_READY || g->state == BG_ST_SERVE)
    {
        bg_place_ball_on_paddle();
        return;
    }
    if (g->state != BG_ST_PLAY)
        return;

    // ---- 3) 球移动（原版：ballX += ballSpeedX; ballY += ballSpeedY）----
    float prev_x = g->ball_x;
    float prev_y = g->ball_y;
    g->ball_x += g->ball_vx;
    g->ball_y += g->ball_vy;

    // 左右墙 + 顶部（游戏区上沿）反弹，和原版一致
    if (g->ball_x <= 0.0f)
    {
        g->ball_x = 0.0f;
        g->ball_vx = fabsf(g->ball_vx);
    }
    if (g->ball_x + (float)BG_BALL >= (float)BG_SCREEN_W)
    {
        g->ball_x = (float)(BG_SCREEN_W - BG_BALL);
        g->ball_vx = -fabsf(g->ball_vx);
    }
    if (g->ball_y <= (float)BG_FIELD_Y)
    {
        g->ball_y = (float)BG_FIELD_Y;
        g->ball_vy = fabsf(g->ball_vy);
    }

    // ---- 4) 先查砖块，再查挡板 ----
    if (bg_hit_brick(prev_x, prev_y))
    {
        g->total_ms += BG_STEP_MS;
        return;
    }

    // ---- 5) 挡板接球（原版：命中位置决定水平速度，接一次 +1 分并略微提速）----
    if (g->ball_vy > 0.0f &&
        g->ball_y + (float)BG_BALL >= (float)BG_PADDLE_Y &&
        g->ball_y + (float)BG_BALL <= (float)(BG_PADDLE_Y + BG_PADDLE_H) &&
        g->ball_x + (float)BG_BALL >= g->paddle_pos &&
        g->ball_x <= g->paddle_pos + (float)BG_PADDLE_W)
    {
        // 吸附到挡板上沿（替代原版的 canScore 冷却：位置一旦抬起就不可能重复判定）
        g->ball_y = (float)(BG_PADDLE_Y - BG_BALL);

        // 接一次球略微提速（原版 ballSpeedX += 0.12 * 难度系数）
        g->ball_speed += 0.06f * g->speed_mul;
        if (g->ball_speed > BG_BALL_SPEED_MAX)
            g->ball_speed = BG_BALL_SPEED_MAX;

        // 命中位置 -> 出射角（原版 constrain(hitPos / 5, -6, 6)；
        // 这里改成「保持总速率不变、只改方向」，避免球在高关卡被压成近乎水平）
        float hit_pos = (g->ball_x + (float)BG_BALL / 2.0f) -
                        (g->paddle_pos + (float)BG_PADDLE_W / 2.0f);
        float vx_max = bg_clampf(g->ball_speed * 0.85f, 0.5f, BG_VX_MAX);
        float nvx = bg_clampf(hit_pos / 8.0f, -vx_max, vx_max);
        float nvy2 = g->ball_speed * g->ball_speed - nvx * nvx;
        float nvy = (nvy2 > 0.25f) ? sqrtf(nvy2) : 0.5f;

        g->ball_vx = nvx;
        g->ball_vy = -nvy; // 一定向上
        g->score += 1;
    }

    // ---- 6) 落底判负（原版：ballY >= GAME_HEIGHT）----
    if (g->ball_y >= (float)BG_SCREEN_H)
        bg_lose_life();

    g->total_ms += BG_STEP_MS;
}

// ============================================================================
//  画面同步：只改该改的东西，不做整屏重绘
// ============================================================================
static void bg_sync_objects(void)
{
    bool playing_visible = (g->state != BG_ST_OVER);

    // 挡板（原版 GAME OVER 时不画挡板）
    if (g_paddle)
    {
        bg_set_visible(g_paddle, &g->vis_paddle, playing_visible);
        if (playing_visible)
            lv_obj_set_pos(g_paddle, g->paddle_x, BG_PADDLE_Y);
    }

    // 球
    if (g_ball)
    {
        bg_set_visible(g_ball, &g->vis_ball, playing_visible);
        if (playing_visible)
            lv_obj_set_pos(g_ball, (int)(g->ball_x + 0.5f), (int)(g->ball_y + 0.5f));
    }

    // 拖尾：原版是两段越来越小、越来越暗的圆，位置 = 球心 - 速度 * t * 2
    bool trail_on = (g->state == BG_ST_PLAY);
    if (g_trail[0])
    {
        for (int i = 0; i < 2; i++)
        {
            bg_set_visible(g_trail[i], &g->vis_trail[i], trail_on);
            if (!trail_on)
                continue;

            int t = i + 1;                     // 1 / 2
            int sz = (i == 0) ? 6 : 4;         // 近端大、远端小
            float cx = g->ball_x + (float)BG_BALL / 2.0f;
            float cy = g->ball_y + (float)BG_BALL / 2.0f;
            int tx = (int)(cx - g->ball_vx * (float)t * 2.0f + 0.5f) - sz / 2;
            int ty = (int)(cy - g->ball_vy * (float)t * 2.0f + 0.5f) - sz / 2;
            lv_obj_set_pos(g_trail[i], tx, ty);
        }
    }
}

// 顶部状态行：得分 xxx  生命 x  Lv.x（过关时短暂改成提示）
static void bg_hud_update(void)
{
    if (!g_lb_hud || !lv_obj_is_valid(g_lb_hud))
        return;

    bool flash = (g->level_flash_ms > 0);
    char buf[48];
    if (flash)
        snprintf(buf, sizeof(buf), "下一关   Lv.%d", g->level); // 「下一关」字形齐全
    else
        snprintf(buf, sizeof(buf), "%s %d   %s %d   Lv.%d",
                 g_word_score, g->score, g_word_life, g->lives, g->level);

    if (strcmp(buf, g->hud_shown) != 0)
    {
        lv_label_set_text(g_lb_hud, buf);
        strncpy(g->hud_shown, buf, sizeof(g->hud_shown) - 1);
        g->hud_shown[sizeof(g->hud_shown) - 1] = 0;
    }
    if ((uint8_t)(flash ? 1 : 0) != g->hud_flash_on)
    {
        g->hud_flash_on = flash ? 1 : 0;
        lv_obj_set_style_text_color(g_lb_hud, flash ? BG_C_GOLD : BG_C_TEXT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

// ============================================================================
//  遮罩层（准备 / 暂停 / 结束），盖住游戏区
// ============================================================================
static void bg_show_overlay(const char *title, const char *sub, const char *hint)
{
    if (!g_overlay || !lv_obj_is_valid(g_overlay))
        return;

    lv_label_set_text(g_ov_title, title ? title : "");
    lv_label_set_text(g_ov_sub, sub ? sub : "");
    lv_label_set_text(g_ov_hint, hint ? hint : "");

    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_overlay); // 保证压在所有砖块/球/挡板上面
}

static void bg_hide_overlay(void)
{
    if (g_overlay && lv_obj_is_valid(g_overlay))
        lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

// 开新局：分数/生命/关卡复位，砖块重铺，短暂等待后自动发球
static void bg_new_game(void)
{
    g->score = 0;
    g->lives = BG_LIVES;
    g->level = 1;
    g->speed_mul = 1.0f;
    g->ball_speed = BG_BASE_SPEED;
    g->total_ms = 0;
    g->acc_ms = 0;
    g->level_flash_ms = 0;
    g->hud_flash_on = 0;

    bg_paddle_center();
    bg_reset_bricks();
    bg_hide_overlay();
    bg_serve_start();
    bg_hud_update();
}

// 确认键 / 电源键：开始 / 暂停 / 继续 / 重开
static void bg_on_confirm(void)
{
    switch (g->state)
    {
    case BG_ST_READY:
        bg_new_game();
        break;

    case BG_ST_PLAY:
        g->state = BG_ST_PAUSE;
        g->paddle_vel = 0.0f;
        // 「确认:恢复」字形齐全；标题在缺字时会退化成「不动」
        bg_show_overlay(g_word_pause, "左右:移动", "确认:恢复");
        break;

    case BG_ST_PAUSE:
        g->state = BG_ST_PLAY;
        bg_hide_overlay();
        break;

    case BG_ST_SERVE:
        bg_launch(); // 不想等就立刻发球
        break;

    case BG_ST_OVER:
        bg_new_game();
        break;

    default:
        break;
    }
}

// 滚轮左右：只改目标位置，真正的位移交给固定步长里的滤波（原版也是这样）
static void bg_paddle_move(int step)
{
    if (!g || g->state == BG_ST_OVER)
        return;

    g->paddle_target += (float)step;
    if (g->paddle_target < 0.0f)
        g->paddle_target = 0.0f;
    if (g->paddle_target > (float)(BG_SCREEN_W - BG_PADDLE_W))
        g->paddle_target = (float)(BG_SCREEN_W - BG_PADDLE_W);
}

// ============================================================================
//  心跳：固定步长推进物理 + 同步画面（绝不 sleep / 死循环）
// ============================================================================
static void bg_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g || !g_root)
        return;

    uint32_t now = lv_tick_get();
    uint32_t dt = lv_tick_elaps(g->last_tick);
    g->last_tick = now;
    if (dt > 150)
        dt = 150; // 被别的界面长时间占住时不要一次补太多步

    // 先消费按键命令（开始/暂停/继续/重开）：只有这里会动 lv_obj，保证在 LVGL 线程
    if (g_pending_cmd)
    {
        g_pending_cmd = 0;
        bg_on_confirm();
    }

    // 倒计时（不参与物理步长）
    if (g->level_flash_ms > 0)
    {
        g->level_flash_ms -= (int)dt;
        if (g->level_flash_ms < 0)
            g->level_flash_ms = 0;
    }
    if (g->state == BG_ST_SERVE)
    {
        g->serve_ms -= (int)dt;
        if (g->serve_ms <= 0)
            bg_launch();
    }

    // 固定步长（16ms/步 = 原版一帧），最多补 BG_MAX_STEPS 步
    g->acc_ms += dt;
    int steps = 0;
    while (g->acc_ms >= BG_STEP_MS && steps < BG_MAX_STEPS)
    {
        g->acc_ms -= BG_STEP_MS;
        steps++;
        bg_step();
    }
    if (g->acc_ms > (uint32_t)(BG_STEP_MS * BG_MAX_STEPS))
        g->acc_ms = 0;

    bg_sync_objects();
    bg_hud_update();
}

// ============================================================================
//  界面搭建
// ============================================================================
static void bg_build_hud(lv_obj_t *root)
{
    // 状态行底色 + 一条 1px 分隔线（用方条画，避免容器 padding 影响绝对坐标）
    lv_obj_t *bar = lv_obj_create(root);
    bg_plain(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, BG_SCREEN_W, BG_HUD_H);
    bg_set_bg(bar, BG_C_HUD);

    lv_obj_t *line = lv_obj_create(root);
    bg_plain(line);
    lv_obj_set_pos(line, 0, BG_HUD_H - 1);
    lv_obj_set_size(line, BG_SCREEN_W, 1);
    bg_set_bg(line, BG_C_LINE);

    // 状态行文字：得分 / 生命 / Lv（16px 字体在 22px 行里放得下）
    g_lb_hud = lv_label_create(root);
    lv_obj_set_style_text_font(g_lb_hud, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_lb_hud, BG_C_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(g_lb_hud, 6, 2);
    lv_label_set_text(g_lb_hud, "");
}

static void bg_build_bricks(lv_obj_t *root)
{
    for (int r = 0; r < BG_BRICK_ROWS; r++)
    {
        for (int c = 0; c < BG_BRICK_COLS; c++)
        {
            lv_obj_t *b = lv_obj_create(root);
            bg_plain(b);
            lv_obj_set_size(b, BG_BRICK_W, BG_BRICK_H);
            lv_obj_set_pos(b, BG_BRICK_X0 + c * BG_BRICK_PITCH_X,
                           BG_BRICK_Y0 + r * BG_BRICK_PITCH_Y);
            bg_set_bg(b, lv_color_hex(kBgBrickColors[r]));
            lv_obj_set_style_radius(b, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
            // 一圈 30% 白描边，做出砖块的立体感
            lv_obj_set_style_border_width(b, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF),
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(b, LV_OPA_30, LV_PART_MAIN | LV_STATE_DEFAULT);

            g->brick_obj[r][c] = b;
        }
    }
}

static void bg_build_paddle(lv_obj_t *root)
{
    g_paddle = lv_obj_create(root);
    bg_plain(g_paddle);
    lv_obj_set_size(g_paddle, BG_PADDLE_W, BG_PADDLE_H);
    lv_obj_set_pos(g_paddle, (BG_SCREEN_W - BG_PADDLE_W) / 2, BG_PADDLE_Y);
    bg_set_bg(g_paddle, BG_C_PADDLE);
    lv_obj_set_style_radius(g_paddle, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 高光条做成挡板的子对象：父对象一动它就跟着动，不用单独同步
    lv_obj_t *hi = lv_obj_create(g_paddle);
    bg_plain(hi);
    lv_obj_set_size(hi, BG_PADDLE_W - 8, 2);
    lv_obj_set_pos(hi, 4, 2);
    bg_set_bg(hi, BG_C_PADDLE_HI);
    lv_obj_set_style_radius(hi, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void bg_build_ball(lv_obj_t *root)
{
    // 拖尾两段（先建，behind 球）
    for (int i = 0; i < 2; i++)
    {
        int sz = (i == 0) ? 6 : 4;
        lv_obj_t *t = lv_obj_create(root);
        bg_plain(t);
        lv_obj_set_size(t, sz, sz);
        bg_set_bg(t, i == 0 ? BG_C_TRAIL0 : BG_C_TRAIL1);
        lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        g_trail[i] = t;
    }

    g_ball = lv_obj_create(root);
    bg_plain(g_ball);
    lv_obj_set_size(g_ball, BG_BALL, BG_BALL);
    bg_set_bg(g_ball, BG_C_BALL);
    lv_obj_set_style_radius(g_ball, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void bg_build_overlay(lv_obj_t *root)
{
    g_overlay = lv_obj_create(root);
    bg_plain(g_overlay);
    lv_obj_set_pos(g_overlay, 0, BG_FIELD_Y);
    lv_obj_set_size(g_overlay, BG_SCREEN_W, BG_FIELD_H);
    lv_obj_set_style_bg_color(g_overlay, BG_C_OVERLAY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_overlay, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(g_overlay, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    g_ov_title = lv_label_create(g_overlay);
    lv_obj_set_style_text_font(g_ov_title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_ov_title, BG_C_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_ov_sub = lv_label_create(g_overlay);
    lv_obj_set_style_text_font(g_ov_sub, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_ov_sub, BG_C_DIM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(g_ov_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_ov_sub, BG_SCREEN_W - 40);
    lv_obj_set_style_text_align(g_ov_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_ov_hint = lv_label_create(g_overlay);
    lv_obj_set_style_text_font(g_ov_hint, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(g_ov_hint, BG_C_GOLD, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// ============================================================================
//  插件导出接口
// ============================================================================
extern "C" const lios_app_info_t lios_app_info = {
    "ball",     // id（权限存储用）
    "彩色块",   // 桌面显示名（字体里没有「弹/球/游/戏」，桌面上直接显示 info.name，
                //           不会走 bg_t 的缺字回退，所以这里必须用字体里有的字）
    "块",       // 图标
    20,         // 排序（小的在前）
    1           // 需要独占整屏
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    // 保险：万一桌面没调 destroy 就又进来一次
    if (g)
    {
        free(g);
        g = NULL;
    }

    g = (bg_game_t *)calloc(1, sizeof(bg_game_t));
    if (!g)
        return NULL;

    // 简易随机种子（不用 rand()，省一次 libc 依赖）
    g->rng = (uint32_t)time(NULL) ^ 0x9E3779B9u;
    if (g->rng == 0)
        g->rng = 0x12345678u;

    // 字形探测：ui_font_cn16 缺「得/分/暂/停/弹/球…」，缺了就换成字体里有的说法
    g_word_title = bg_t("弹球游戏", "PONG");
    g_word_score = bg_t("得分", "计数");
    g_word_life = bg_t("生命", "命数");
    g_word_pause = bg_t("暂停", "不动");
    g_word_over = bg_t("游戏结束", "结束");

    g_root = NULL;
    g_lb_hud = NULL;
    g_paddle = NULL;
    g_ball = NULL;
    g_trail[0] = g_trail[1] = NULL;
    g_overlay = g_ov_title = g_ov_sub = g_ov_hint = NULL;

    // 最外层容器：铺满 320x240，纯黑，不滚动
    g_root = lv_obj_create(parent);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, BG_SCREEN_W, BG_SCREEN_H);
    bg_set_bg(g_root, BG_C_FIELD);
    lv_obj_set_style_border_width(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(g_root, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 初始状态：等确认键开始
    g->state = BG_ST_READY;
    g->score = 0;
    g->lives = BG_LIVES;
    g->level = 1;
    g->speed_mul = 1.0f;
    g->ball_speed = BG_BASE_SPEED;
    g->bricks_left = 0;
    g->last_tick = lv_tick_get();
    g->vis_paddle = g->vis_ball = BG_VIS_UNKNOWN;
    g->vis_trail[0] = g->vis_trail[1] = BG_VIS_UNKNOWN;
    g->hud_flash_on = 0;
    g->hud_shown[0] = 0;
    g_pending_cmd = 0;

    bg_paddle_center();
    bg_place_ball_on_paddle();

    // 建控件：状态行 -> 砖块 -> 挡板 -> 球 -> 遮罩（顺序 = 图层顺序）
    bg_build_hud(g_root);
    bg_build_bricks(g_root);
    bg_build_paddle(g_root);
    bg_build_ball(g_root);
    bg_build_overlay(g_root);

    bg_reset_bricks();
    bg_hud_update();
    bg_sync_objects();
    bg_show_overlay(g_word_title, bg_t("滚轮左右:移动挡板", "左右:移动"), "确认:开始");

    // 游戏心跳：30ms 一次，物理内部按 16ms 固定步长补，绝不 sleep
    g_timer = lv_timer_create(bg_timer_cb, BG_TIMER_MS, NULL);

    return g_root;
}

extern "C" void lios_app_destroy(void)
{
    // 先停心跳，避免 LVGL 之后再回调到已经释放的数据
    if (g_timer)
    {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
    g_pending_cmd = 0;

    // LVGL 对象由桌面容器统一删除，这里只把本地指针清空（防止野指针）
    g_root = NULL;
    g_lb_hud = NULL;
    g_paddle = NULL;
    g_ball = NULL;
    g_trail[0] = g_trail[1] = NULL;
    g_overlay = g_ov_title = g_ov_sub = g_ov_hint = NULL;

    // 释放自己 calloc 的游戏数据（砖块对象指针数组在这个结构体里，一起回收）
    if (g)
    {
        free(g);
        g = NULL;
    }
}

extern "C" void lios_app_key(int key, int event)
{
    if (!g || !g_root)
        return;

    // 抬起事件不用管：滚轮每转一格就是一次「按下」，没有长按连发需求
    if (event == LIOS_EV_RELEASE)
        return;

    switch (key)
    {
    case LIOS_KEY_LEFT:
    case LIOS_KEY_UP: // 兜底：个别固件把滚轮上报成上下，也当左右用
        bg_paddle_move(-BG_KEY_STEP);
        break;

    case LIOS_KEY_RIGHT:
    case LIOS_KEY_DOWN:
        bg_paddle_move(+BG_KEY_STEP);
        break;

    case LIOS_KEY_ENTER:
    case LIOS_KEY_POWER:
        // 只在「按下」时响应，按住确认键不会连续重开；
        // 这里只置标志，真正动控件交给 bg_timer_cb（LVGL 线程）
        if (event == LIOS_EV_PRESS)
            g_pending_cmd = 1;
        break;

    case LIOS_KEY_BACK:
    default:
        // BACK 交给桌面统一处理退出，这里不拦截
        break;
    }
}
