// LiOS 第二期：真正的桌面 —— 方块网格 + 插件框架（dlopen）+ 双向模式切换
//
// 这一版把第一期的「建设中」占位桌面换掉：
//   * 3 列 × 3 行方块网格，一格一个应用，滚轮移动高亮、确认进入（超出可上下滚动）
//   * 扫描 /mnt/UDISK/apps/*.so，按 include/lios_app.h 的 ABI 读 lios_app_info，
//     合并进网格；加载失败/缺符号/ABI 不符 → 写日志跳过，绝不让桌面崩
//   * 选中插件 → 建全屏容器 → lios_app_create(container) → 按键转发 lios_app_key()
//     → 退出时 lios_app_destroy() + dlclose()
//   * 内置「文件管理器 / 设置 / 权限管理 / 后台」本期是占位页（下一期实现）
//   * 网格里有「相机」方块 → 切回相机；相机界面里在隐藏 SH 终端按「进 LiOS」切过来
//
// 线程模型见 include/lios_launcher.h。

#include "lios_launcher.h"
#include "lios_user.h"
#include "my_main.h"
#include "statusBar.h" // ui_statusbar_create()
#include "lios_app.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// main.cpp 里的线程（launcher 线程结束后不 join，交给 main 的 pthread_join 兜底）
extern pthread_t thread_app;
extern pthread_t thread_temp_poll;
extern pthread_t thread_center_temp;
extern pthread_t thread_ui;
void *thread_app_func(void *);
void *thread_temp_poll_func(void *);
void *thread_center_temp_func(void *);
void *thread_ui_func(void *);

// ---------------------------------------------------------------- 常量

#define LIOS_APPS_DIR "/mnt/UDISK/apps"
#define LIOS_APP_MAX 16 // 最多几个应用（内置 + 插件）

#define LIOS_TILE_W 84
#define LIOS_TILE_H 60
#define LIOS_TILE_GAP_X 4
#define LIOS_TILE_GAP_Y 4
#define LIOS_GRID_COLS 3
#define LIOS_GRID_X 30 // 3*84 + 2*4 = 260；(320-260)/2 = 30
#define LIOS_GRID_Y 26
#define LIOS_GRID_H 208

// 应用来源
enum
{
    LIOS_SRC_BUILTIN = 0, // 内置（普通占位页 / 回相机）
    LIOS_SRC_PLUGIN = 1,  // /mnt/UDISK/apps/*.so
};

typedef struct
{
    char id[32];
    char name[24];  // 显示名
    char icon[8];   // 1~2 个汉字/符号
    int order;
    int source;
    void *handle; // 插件：dlopen 句柄；内置：NULL
} lios_app_t;

// 内置应用：id / 名字 / 图标 / 排序
static const struct
{
    const char *id;
    const char *name;
    const char *icon;
    int order;
} LIOS_BUILTINS[] = {
    {"camera", "相机", "相", 0},
    {"files", "文件管理", "文", 10},
    {"settings", "设置", "设", 20},
    {"perm", "权限管理", "权", 30},
    {"tasks", "后台", "后", 40},
};

// ---------------------------------------------------------------- 全局状态

static lios_app_t g_apps[LIOS_APP_MAX];
static int g_app_cnt = 0;
static bool g_active = false;          // 桌面是否正在显示
static bool g_quit = false;            // 桌面要不要退出（回相机）
static bool g_app_running = false;     // 有没有应用占着屏幕
static int g_app_index = -1;           // 正在运行的应用下标
static bool g_enter_requested = false; // 被请求切进 LiOS
static volatile bool g_app_exit_request = false; // 应用自己要求退出（插件用）
static volatile bool g_camera_ready = false;     // enter_camera 是否已经把 UI 线程建出来了
static bool g_debug_force_lios = false;          // --debug-lios 直接进桌面（调试用，跳过密码/手势）

// 调试入口：`LiThermal --debug-lios` 直接进 LiOS 桌面（不用输密码）。
// 只给开发/排查用，正常启动（thermalcamera.sh 用 --login）不受影响。
bool lios_debug_lios_requested(int argc, char **argv)
{
    g_debug_force_lios = false;
    for (int i = 1; i < argc; i++)
        if (argv[i] != NULL && strcmp(argv[i], "--debug-lios") == 0)
            g_debug_force_lios = true;
    if (g_debug_force_lios)
        lios_log("--debug-lios：跳过登录，直接进 LiOS 桌面（调试用）");
    return g_debug_force_lios;
}

static lv_obj_t *scr_lios = NULL;      // 桌面根容器（lv_layer_sys，全屏不透明）
static lv_obj_t *grid_cont = NULL;     // 方块网格容器
static lv_obj_t *tile_objs[LIOS_APP_MAX] = {NULL};
static lv_obj_t *app_cont = NULL;      // 应用的全屏容器
static lv_obj_t *hint_lbl = NULL;      // 底部提示行

// 应用占屏时把编码器输入整个关掉：这样 LVGL 的 group 焦点不会被滚轮改到别处
// （内置占位页自己不处理按键，转发也是空转），退出应用时再打开。
// lv_indev_enable(NULL, ...) 的语义就是「所有输入设备」，正好用。
static void lios_indev_disable_for_app(void)
{
    lv_indev_enable(NULL, false);
}

static void lios_indev_enable_after_app(void)
{
    lv_indev_enable(NULL, true);
}

// ---------------------------------------------------------------- 小工具

static bool ends_with(const char *s, const char *suffix)
{
    if (s == NULL || suffix == NULL)
        return false;
    size_t ls = strlen(s), lf = strlen(suffix);
    return (ls >= lf) && (strcmp(s + ls - lf, suffix) == 0);
}

static void app_clear(int i)
{
    if (i < 0 || i >= LIOS_APP_MAX)
        return;
    memset(&g_apps[i], 0, sizeof(g_apps[i]));
    g_apps[i].order = 1000;
}

static bool app_add(const char *id, const char *name, const char *icon, int order, int source, void *handle)
{
    if (g_app_cnt >= LIOS_APP_MAX)
        return false;
    lios_app_t *a = &g_apps[g_app_cnt];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id ? id : "?");
    snprintf(a->name, sizeof(a->name), "%s", name ? name : a->id);
    snprintf(a->icon, sizeof(a->icon), "%s", (icon && icon[0]) ? icon : "应");
    a->order = order;
    a->source = source;
    a->handle = handle;
    g_app_cnt++;
    return true;
}

// ---------------------------------------------------------------- 插件扫描

typedef const lios_app_info_t *(*lios_info_fn)(void);
typedef lv_obj_t *(*lios_create_fn)(lv_obj_t *);
typedef void (*lios_void_fn)(void);
typedef void (*lios_key_fn)(int, int);

static void scan_builtins(void)
{
    for (size_t i = 0; i < sizeof(LIOS_BUILTINS) / sizeof(LIOS_BUILTINS[0]); i++)
        app_add(LIOS_BUILTINS[i].id, LIOS_BUILTINS[i].name, LIOS_BUILTINS[i].icon,
                LIOS_BUILTINS[i].order, LIOS_SRC_BUILTIN, NULL);
}

// 扫 /mnt/UDISK/apps/*.so；目录不存在 = 正常情况（只有内置项）
static void scan_plugins(void)
{
    DIR *d = opendir(LIOS_APPS_DIR);
    if (d == NULL)
    {
        lios_log("插件目录 %s 不存在，只显示内置应用", LIOS_APPS_DIR);
        return;
    }

    int loaded = 0, failed = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (!ends_with(ent->d_name, ".so"))
            continue;

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", LIOS_APPS_DIR, ent->d_name);

        void *h = dlopen(path, RTLD_NOW);
        if (h == NULL)
        {
            const char *err = dlerror(); // 注意 dlerror() 是一次性的，只能取一次
            lios_log("跳过 %s：dlopen 失败 %s", ent->d_name, err ? err : "?");
            failed++;
            continue;
        }

        // 信息结构体有两种导出方式都支持：变量 lios_app_info / 函数 lios_app_info_get()
        const lios_app_info_t *info = NULL;
        void *sym = dlsym(h, "lios_app_info");
        if (sym != NULL)
            info = (const lios_app_info_t *)sym;
        if (info == NULL)
        {
            lios_info_fn fn = (lios_info_fn)dlsym(h, "lios_app_info_get");
            if (fn != NULL)
                info = fn();
        }
        // 三个函数必须有，否则 ABI 不符
        lios_create_fn f_create = (lios_create_fn)dlsym(h, "lios_app_create");
        lios_void_fn f_destroy = (lios_void_fn)dlsym(h, "lios_app_destroy");
        lios_key_fn f_key = (lios_key_fn)dlsym(h, "lios_app_key");

        if (info == NULL || f_create == NULL || f_destroy == NULL || f_key == NULL ||
            info->id == NULL || info->name == NULL || info->id[0] == '\0')
        {
            lios_log("跳过 %s：ABI 不符（info=%p create=%p destroy=%p key=%p）",
                     ent->d_name, (void *)info, (void *)f_create, (void *)f_destroy, (void *)f_key);
            dlclose(h);
            failed++;
            continue;
        }

        // id 重复的跳过（同一插件装了两份）
        bool dup = false;
        for (int i = 0; i < g_app_cnt; i++)
            if (strcmp(g_apps[i].id, info->id) == 0)
                dup = true;
        if (dup)
        {
            lios_log("跳过 %s：id 重复（%s）", ent->d_name, info->id);
            dlclose(h);
            failed++;
            continue;
        }

        if (app_add(info->id, info->name, info->icon, info->order, LIOS_SRC_PLUGIN, h))
        {
            lios_log("加载插件 %s：id=%s name=%s icon=%s", ent->d_name, info->id, info->name,
                     info->icon ? info->icon : "");
            loaded++;
        }
        else
        {
            lios_log("跳过 %s：应用数量已达上限 %d", ent->d_name, LIOS_APP_MAX);
            dlclose(h);
            failed++;
        }
    }
    closedir(d);
    lios_log("插件扫描完成：成功 %d 个，跳过 %d 个", loaded, failed);
}

// 排序（order 小的在前，内置相机 order=0 永远第一；同 order 保持原顺序）
static void sort_apps(void)
{
    for (int i = 1; i < g_app_cnt; i++)
    {
        lios_app_t tmp = g_apps[i];
        int j = i - 1;
        while (j >= 0 && g_apps[j].order > tmp.order)
        {
            g_apps[j + 1] = g_apps[j];
            j--;
        }
        g_apps[j + 1] = tmp;
    }
}

// ---------------------------------------------------------------- 插件调用

static bool plugin_create(void)
{
    if (g_app_index < 0 || g_app_index >= g_app_cnt)
        return false;
    lios_app_t *a = &g_apps[g_app_index];
    if (a->source != LIOS_SRC_PLUGIN || a->handle == NULL)
        return false;
    lios_create_fn f = (lios_create_fn)dlsym(a->handle, "lios_app_create");
    if (f == NULL)
        return false;
    LOCKLV();
    f(app_cont);
    UNLOCKLV();
    lios_log("插件 %s：lios_app_create 完成", a->id);
    return true;
}

static void plugin_destroy(void)
{
    if (g_app_index < 0 || g_app_index >= g_app_cnt)
        return;
    lios_app_t *a = &g_apps[g_app_index];
    if (a->source != LIOS_SRC_PLUGIN || a->handle == NULL)
        return;
    lios_void_fn f = (lios_void_fn)dlsym(a->handle, "lios_app_destroy");
    if (f != NULL)
        f(); // 插件自己清理 malloc 的内存
    lios_log("插件 %s：lios_app_destroy 完成", a->id);
}

static void plugin_key(int key, int event)
{
    if (g_app_index < 0 || g_app_index >= g_app_cnt)
        return;
    lios_app_t *a = &g_apps[g_app_index];
    if (a->source != LIOS_SRC_PLUGIN || a->handle == NULL)
        return;
    lios_key_fn f = (lios_key_fn)dlsym(a->handle, "lios_app_key");
    if (f != NULL)
        f(key, event);
}

// ---------------------------------------------------------------- 界面

static void set_hint(const char *s)
{
    if (hint_lbl != NULL && lv_obj_is_valid(hint_lbl))
        lv_label_set_text(hint_lbl, s ? s : "");
}

// 打开下标 idx 的方块（tile_click_cb 和自动测试都走这一条路径）
// 注意：**调用时不能已经持有 lv_mutex**（内部会 LOCKLV）
static void tile_activate_locked(int idx);
static void app_exit_locked(void);
static void tile_activate(int idx)
{
    LOCKLV();
    tile_activate_locked(idx);
    UNLOCKLV();
}

// ---------------------------------------------------------------- 自动测试（--debug-lios + /tmp/lios_autotest）
//
// 目的：不用人按，自己跑「进桌面 -> 打开方块 -> 心跳」，
// 每 500ms 打一条心跳来证明 UI 线程是否还活着（用来区分「死锁卡死」和「崩溃」）。
// 默认关闭：只有 /tmp/lios_autotest 存在 + 启动参数带 --debug-lios 才会跑。

static volatile uint32_t g_ui_beat = 0; // UI 线程每转一圈 +1（HAL::lv_loop 里喂）
static volatile int g_autotest_step = 0; // 自动测试走到第几步
static volatile int g_tile_open_cnt = 0; // 「打开方块」成功执行了几次
static volatile int g_app_exit_cnt = 0;  // 「退出应用」成功执行了几次
static volatile bool g_autotest_wanted = false; // 自动测试是否被请求（/tmp/lios_autotest 存在）
static volatile bool g_autotest_armed_done = false; // 定时器是否已经建过

// HAL::lv_loop 每圈调一次（myHAL.cpp）——看门狗就看这个计数器
void lios_ui_beat(void)
{
    g_ui_beat++;
}

// 定时器回调里只改内存计数，不写日志（实测在定时器回调里做文件 I/O 会不可靠）
static void autotest_step_cb(lv_timer_t *t)
{
    g_autotest_step++;
    switch (g_autotest_step)
    {
    case 1: // 打开第一个内置方块（「文件管理」）
        tile_activate_locked(1);
        g_tile_open_cnt++;
        break;
    case 2:
        app_exit_locked();
        g_app_exit_cnt++;
        break;
    case 3:
        lv_timer_del(t);
        break;
    default:
        break;
    }
}

void lios_autotest_start(void)
{
    if (access("/tmp/lios_autotest", F_OK) != 0)
        return;
    lios_log("=== autotest 已启用（/tmp/lios_autotest 存在）===");
    // 这里**只登记意图**，真正的 lv_timer_create 交给 HAL 线程在桌面显示期间做
    //（必须等 UI 线程跑起来；实测 UI 线程起来之前建的 LVGL 定时器不会被触发）
    g_autotest_wanted = true;
}

// 由看门狗线程 / HAL 线程在 UI 线程跑起来之后调用（一次性）
static void autotest_arm_timers(void)
{
    static bool armed = false;
    if (armed)
        return;
    // 每次 arm 都重新确认一次标志文件（不依赖主线程那次 access 的时序）
    if (access("/tmp/lios_autotest", F_OK) != 0 && !g_autotest_wanted)
        return;
    armed = true;
    g_autotest_armed_done = true;
    LOCKLV();
    lv_timer_create(autotest_step_cb, 3000, NULL);
    UNLOCKLV();
    lios_log("autotest: 已启动（3 秒后自动打开第一个方块，6 秒后退出应用）");
}

// ---------------------------------------------------------------- 看门狗
//
// 安全网：如果哪一步又把 UI 线程卡死了（屏幕不刷新、按键不响应），看门狗会写一条
// 日志进 /tmp/lios.log，然后 _exit(99) 让 thermalcamera.sh 的降级循环把相机重新拉起来
//（宁可重启，也不要留一块死屏 + 电源键无反应）。
//
// 心跳来源是 HAL::lv_loop 每圈调的 lios_ui_beat()（纯计数器）；
// 故意不用 LVGL 定时器：实测 UI 线程起来之前建的 LVGL 定时器根本不会触发。

static volatile bool g_wd_stop = false;
static pthread_t g_wd_thread;

static void *lios_watchdog_thread(void *)
{
    uint32_t last = g_ui_beat;
    int stuck = 0;
    int tick = 0;
    // 等 UI 线程跑起来，再把自动测试的定时器建出来
    // （同样是因为「UI 线程起来之前建的 LVGL 定时器不会触发」这个坑）
    usleep(2000 * 1000);
    autotest_arm_timers();
    while (!g_wd_stop)
    {
        usleep(500 * 1000);
        if (g_wd_stop)
            break;
        if (g_autotest_wanted && ++tick % 4 == 0) // 自动测试时才每 2 秒报一次进度
            lios_log("看门狗: ui_beat=%u step=%d tile_open=%d app_exit=%d",
                     (unsigned)g_ui_beat, g_autotest_step, g_tile_open_cnt, g_app_exit_cnt);
        if (g_ui_beat == last)
        {
            stuck++;
            if (stuck >= 6) // 3 秒
            {
                lios_log("!!! 看门狗：UI 心跳停了 3 秒（疑似死锁），强制退出让脚本重启相机");
                printf("[lios] watchdog timeout -> _exit(99)\n");
                sync();
                _exit(99);
            }
        }
        else
        {
            stuck = 0;
            last = g_ui_beat;
        }
    }
    return NULL;
}

void lios_watchdog_start(void)
{
    g_wd_stop = false;
    if (pthread_create(&g_wd_thread, NULL, lios_watchdog_thread, NULL) == 0)
    {
        pthread_detach(g_wd_thread);
        lios_log("看门狗已启动（心跳来自 lv_loop，卡死 3 秒即重启相机）");
    }
    else
    {
        lios_log("看门狗线程创建失败（不影响使用）");
    }
}

static void tile_focus_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    if (t != NULL && lv_obj_is_valid(t))
        lv_obj_scroll_to_view(t, LV_ANIM_ON);
}

static void app_exit(void);

// 打开下标 idx 的方块：内部版（调用者必须已经持有 lv_mutex）
//
// 之所以拆成 locked / unlocked 两个版本：
//   * tile_click_cb 是 LVGL 的**事件回调**，它是在 lv_timer_handler() 里被派发的，
//     而 lv_timer_handler() 是在 HAL::lv_loop() 的 LOCKLV() 里面调的 ——
//     也就是说**事件回调里 lv_mutex 已经被自己持有了**。
//   * lv_mutex 是普通（非递归）互斥锁，在回调里再 LOCKLV() 会**自锁死**：
//     UI 线程卡住 → 屏幕不再刷新、按键不再处理，连 HAL 线程的电源键处理
//     （refresh_poweroff_key → lv_ui_poweroff 也要拿这把锁）也一起卡死，
//     表现就是「点一下方块整机卡死、长按电源键无反应」。← 第二期实测就是这个 bug
// 所以：LVGL 回调/定时器里一律只调 *_locked 版本，**绝不再 LOCKLV**。
static void tile_activate_locked(int idx)
{
    if (idx < 0 || idx >= g_app_cnt)
        return;

    if (strcmp(g_apps[idx].id, "camera") == 0)
    {
        // 「相机」方块 = 切回相机（和菜单键等效）
        lios_log("桌面：选择相机方块 -> 回相机");
        g_quit = true;
        return;
    }

    g_app_index = idx;
    g_app_running = true;
    lios_log("桌面：进入应用 %s（source=%s）", g_apps[idx].id,
             g_apps[idx].source == LIOS_SRC_PLUGIN ? "plugin" : "builtin");

    app_cont = lv_obj_create(lv_layer_sys());
    lios_log("桌面：应用容器 %p 已建", (void *)app_cont);
    lv_obj_set_size(app_cont, 320, 240);
    lv_obj_set_pos(app_cont, 0, 0);
    lv_obj_clear_flag(app_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(app_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(app_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(app_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(app_cont, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(app_cont, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(app_cont);

    if (g_apps[idx].source == LIOS_SRC_BUILTIN)
    {
        // 内置应用本期是占位页（下一期做文件管理器/设置/权限/后台）
        lv_obj_t *t1 = lv_label_create(app_cont);
        lv_label_set_text(t1, g_apps[idx].name);
        lv_obj_set_style_text_font(t1, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(t1, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_t *t2 = lv_label_create(app_cont);
        lv_label_set_text(t2, "建设中");
        lv_obj_set_style_text_font(t2, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(t2, lv_color_hex(0x9AA7B4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(t2, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t *t3 = lv_label_create(app_cont);
        lv_label_set_text(t3, "菜单键返回桌面");
        lv_obj_set_style_text_font(t3, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(t3, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(t3, LV_ALIGN_BOTTOM_MID, 0, -8);
        lios_log("桌面：内置占位页建完");
    }
    else if (!plugin_create())
    {
        // 插件当场挂了：不要停在黑屏，直接退回桌面
        lios_log("插件 %s：create 调用失败，退回桌面", g_apps[idx].id);
        app_exit_locked();
        return;
    }

    set_hint("菜单键返回桌面");
    lios_indev_disable_for_app(); // 应用占屏期间不让滚轮改桌面焦点
    lios_log("桌面：进入应用完成");
}

// 确认进入某个方块（LVGL 事件回调：已在锁内，绝不能再 LOCKLV）
static void tile_click_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(t);
    tile_activate_locked(idx);
}

// 退出当前应用的**内部版**（调用者必须已经持有 lv_mutex：LVGL 回调/定时器里用）
static void app_exit_locked(void)
{
    if (!g_app_running)
        return;
    g_app_exit_request = false;
    lios_app_t *a = (g_app_index >= 0 && g_app_index < g_app_cnt) ? &g_apps[g_app_index] : NULL;

    if (a != NULL && a->source == LIOS_SRC_PLUGIN)
    {
        plugin_key(LIOS_KEY_BACK, LIOS_EV_PRESS);
        plugin_destroy();
    }

    if (app_cont != NULL && lv_obj_is_valid(app_cont))
    {
        // 先把焦点挪回一个还活着的方块，再删应用容器，
        // 避免 LVGL 的 group 里留下指向已删对象的焦点
        if (tile_objs[0] != NULL && lv_obj_is_valid(tile_objs[0]))
            lv_group_focus_obj(tile_objs[0]);
        lv_obj_del(app_cont);
    }
    app_cont = NULL;
    lios_indev_enable_after_app();

    // 插件用一次卸一次，释放内存（内置的不需要）
    if (a != NULL && a->source == LIOS_SRC_PLUGIN && a->handle != NULL)
    {
        dlclose(a->handle);
        a->handle = NULL; // 下次再进需要重启程序重新加载
        lios_log("插件 %s：dlclose 完成", a->id);
    }

    g_app_running = false;
    g_app_index = -1;
    set_hint("滚轮选择  按下进入  菜单键回相机");
    lios_log("桌面：已退出应用");
}

// 退出当前应用（外部调用版：自己加锁）
static void app_exit(void)
{
    LOCKLV();
    app_exit_locked();
    UNLOCKLV();
}

static void build_grid(void)
{
    grid_cont = lv_obj_create(scr_lios);
    lv_obj_set_size(grid_cont, 320, LIOS_GRID_H);
    lv_obj_set_pos(grid_cont, 0, LIOS_GRID_Y);
    lv_obj_set_style_bg_opa(grid_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(grid_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(grid_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(grid_cont, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < g_app_cnt; i++)
    {
        int col = i % LIOS_GRID_COLS;
        int row = i / LIOS_GRID_COLS;

        lv_obj_t *t = lv_btn_create(grid_cont);
        lv_obj_set_size(t, LIOS_TILE_W, LIOS_TILE_H);
        lv_obj_set_pos(t, LIOS_GRID_X + col * (LIOS_TILE_W + LIOS_TILE_GAP_X),
                       row * (LIOS_TILE_H + LIOS_TILE_GAP_Y) + 4);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_radius(t, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x18222C), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(t, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(t, lv_color_hex(0x2B3640), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *ic = lv_label_create(t);
        lv_label_set_text(ic, g_apps[i].icon);
        lv_obj_set_style_text_font(ic, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ic, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);

        lv_obj_t *nm = lv_label_create(t);
        lv_label_set_text(nm, g_apps[i].name);
        lv_obj_set_style_text_font(nm, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -8);

        lv_obj_set_user_data(t, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(t, tile_focus_cb, LV_EVENT_FOCUSED, NULL);
        tile_objs[i] = t;

        if (i == 0)
            lv_group_focus_obj(t);
    }
}

// ---------------------------------------------------------------- 桌面生命周期

void lios_launcher_screen(void)
{
    g_app_cnt = 0;
    g_app_running = false;
    g_app_index = -1;
    g_quit = false;
    g_app_exit_request = false;

    lios_log("进入 LiOS 桌面");
    scan_builtins();
    scan_plugins();
    sort_apps();
    lios_log("桌面应用数：%d", g_app_cnt);

    LOCKLV();
    lios_log("桌面: 开始建界面");
    scr_lios = lv_obj_create(lv_layer_sys());
    lios_log("桌面: 根容器 %p", (void *)scr_lios);
    lv_obj_set_size(scr_lios, 320, 240);
    lv_obj_set_pos(scr_lios, 0, 0);
    lv_obj_clear_flag(scr_lios, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr_lios, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(scr_lios, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(scr_lios, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scr_lios, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr_lios, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(scr_lios);
    lv_label_set_text(title, "LiOS");
    lv_obj_set_style_text_font(title, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title, 6, 4);

    hint_lbl = lv_label_create(scr_lios);
    lv_obj_set_style_text_font(hint_lbl, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(hint_lbl, "滚轮选择  按下进入  菜单键回相机");

    build_grid();
    lios_log("桌面: 网格已建好");
    lv_obj_move_foreground(scr_lios); // 盖住相机界面（状态条在右上角仍可见）
    UNLOCKLV();
    lios_log("桌面: 界面完成，开始等待按键");

    g_active = true;

    // 等退出：菜单键（且没有应用在跑）或者「相机」方块
    while (!g_quit)
    {
        bool menu = false;
        LOCKLV();
        if (HAL::key_press_event[1])
        {
            HAL::key_press_event[1] = false;
            menu = true;
        }
        UNLOCKLV();
        if (menu && !g_app_running)
        {
            lios_log("桌面：菜单键 -> 回相机");
            g_quit = true;
        }
        usleep(20000);
    }

    // 收尾：先退应用，再删桌面
    if (g_app_running)
        app_exit();

    g_active = false;

    LOCKLV();
    if (tile_objs[0] != NULL && lv_obj_is_valid(tile_objs[0]))
        lv_group_focus_obj(tile_objs[0]); // 焦点别留在要删的对象上
    lios_indev_enable_after_app();        // 防御：确保输入没被关掉
    if (scr_lios != NULL && lv_obj_is_valid(scr_lios))
        lv_obj_del(scr_lios);
    scr_lios = NULL;
    grid_cont = NULL;
    hint_lbl = NULL;
    for (int i = 0; i < LIOS_APP_MAX; i++)
        tile_objs[i] = NULL;
    app_cont = NULL;
    UNLOCKLV();

    // 剩下的插件句柄统一释放
    for (int i = 0; i < g_app_cnt; i++)
    {
        if (g_apps[i].handle != NULL)
        {
            dlclose(g_apps[i].handle);
            g_apps[i].handle = NULL;
        }
    }
    for (int i = 0; i < LIOS_APP_MAX; i++)
        app_clear(i);
    g_app_cnt = 0;

    lios_log("退出 LiOS 桌面");
}

// ---------------------------------------------------------------- 线程 / 模式

static void *lios_launcher_thread(void *)
{
    g_enter_requested = false;
    current_mode = MODE_LIOS;
    lios_launcher_screen();
    enter_camera();
    return NULL;
}

void lios_enter(void)
{
    if (g_enter_requested || g_active)
    {
        lios_log("lios_enter：已经在 LiOS 里，忽略");
        return;
    }
    g_enter_requested = true;

    pthread_t th;
    if (pthread_create(&th, NULL, lios_launcher_thread, NULL) != 0)
    {
        // 线程都起不来就老老实实回相机，不能把用户困住
        lios_log("lios_enter：pthread_create 失败，放弃进 LiOS");
        g_enter_requested = false;
        current_mode = MODE_MAINPAGE;
        enter_camera();
        return;
    }
    pthread_detach(th);
    lios_log("lios_enter：launcher 线程已起");
}
bool lios_mode_active(void)
{
    return g_active;
}

// main 用：等 UI 线程真的建出来（LiOS 启动路径下是 launcher 线程建的，
// 直接 pthread_join(thread_ui) 有可能 join 到 0，那是未定义行为）
bool lios_camera_ready(void)
{
    return g_camera_ready;
}

bool lios_launcher_app_active(void)
{
    return g_active && g_app_running;
}

void lios_app_request_exit(void)
{
    g_app_exit_request = true;
}

// ---------------------------------------------------------------- 按键转发

void lios_launcher_poll(void)
{
    if (!g_active)
        return;

    // 自动测试：HAL 线程每圈顺手确认一次标志文件（这里调 lv_timer_create 时 UI 线程一定在跑，
    // 定时器才会被触发；arm 内部有一次性保护，不会重复建）
    if (!g_autotest_armed_done)
    {
        static int arm_check = 0;
        if (++arm_check >= 20)
        {
            arm_check = 0;
            autotest_arm_timers();
        }
    }

    // 有应用在跑：滚轮/确认/录像/菜单 全部转发给应用
    if (g_app_running)
    {
        // 插件自己要求退出（例如内置的返回按钮）：由这边统一收尾
        if (g_app_exit_request)
        {
            app_exit();
            return;
        }

        LOCKLV();
        int enc = last_encoder_direction;
        last_encoder_direction = 0;
        bool enter_ev = HAL::key_press_event[3];
        if (enter_ev)
            HAL::key_press_event[3] = false;
        bool rec_ev = HAL::key_press_event[2];
        if (rec_ev)
            HAL::key_press_event[2] = false;
        UNLOCKLV();

        if (enc > 0)
            plugin_key(LIOS_KEY_UP, LIOS_EV_PRESS);
        else if (enc < 0)
            plugin_key(LIOS_KEY_DOWN, LIOS_EV_PRESS);

        if (enter_ev)
        {
            plugin_key(LIOS_KEY_ENTER, LIOS_EV_PRESS);
            plugin_key(LIOS_KEY_ENTER, LIOS_EV_RELEASE);
        }
        if (rec_ev)
        {
            plugin_key(LIOS_KEY_REC, LIOS_EV_PRESS);
            plugin_key(LIOS_KEY_REC, LIOS_EV_RELEASE);
        }
    }
    else
    {
        // 桌面上：滚轮交给 LVGL 走焦点（myHAL.cpp: encoder_read 的判断），
        // 这里只需要清掉 HAL 跟踪用的方向，别让它泄到别的模式
        LOCKLV();
        last_encoder_direction = 0;
        UNLOCKLV();
    }
}

// ---------------------------------------------------------------- 相机启动（幂等）

void enter_camera(void)
{
    static bool camera_started = false;

    // 在 LiOS 里被调用 = 只是「想要回相机」，等桌面退出后 launcher 线程会再调一次
    if (current_mode == MODE_LIOS)
    {
        lios_log("enter_camera：当前在 LiOS，先交给桌面处理");
        return;
    }
    if (camera_started)
        return;
    camera_started = true;

    lios_log("enter_camera：开始启动相机");
    printf("Loop begin\n");
    waitboot_scr_load(lv_scr_act());
    lios_log("enter_camera：waitboot 完成");
    widget_graph_create();
    ui_crosshairs_create();
    ui_center_display_create();
    ui_statusbar_create(); // 顶部半透明状态条（CPU/内存）
    lios_log("enter_camera：界面部件建完");
    if (thread_ui == 0)
    {
        // 注意：LiOS 启动路径下 enter_camera 是在 launcher 线程里跑的，
        // 而 main 最后会 pthread_join(thread_ui) —— 一定要等线程真的建出来
        // 再把 thread_ui 交出去，否则 main 会 join(0)（未定义行为，实测会崩）。
        if (pthread_create(&thread_ui, NULL, thread_ui_func, NULL) == 0)
        {
            lios_log("enter_camera：UI 线程已起");
            lios_watchdog_start(); // UI 线程跑起来了，看门狗才有心跳可看
        }
        else
        {
            lios_log("enter_camera：UI 线程创建失败！");
        }
    }
    cameraUtils.initHTTPClient();
    pthread_create(&thread_app, NULL, thread_app_func, NULL);
    pthread_create(&thread_temp_poll, NULL, thread_temp_poll_func, NULL); // 温度轮询独立线程
    pthread_create(&thread_center_temp, NULL, thread_center_temp_func, NULL);
    current_mode = MODE_MAINPAGE;
    g_camera_ready = true; // 通知 main 可以 join UI 线程了
    lios_log("enter_camera：相机线程已启动");
}
