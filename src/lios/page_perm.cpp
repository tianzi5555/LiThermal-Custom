// LiOS 权限管理器
//
// 每个应用（内置用 app id，插件用 plugin id）一份权限记录：
//     /mnt/UDISK/lios/perm/<id>.conf    内容若干行 key=1 / key=0
// 权限项：storage 读写存储 / camera 相机与视频 / network 网络 / bg 后台运行
//
// 第一版**不做强制拦截**（musl、无沙箱；真正拦截要插件配合），
// 但提供查询接口 lios_perm_allowed(id, key)（主程序 -rdynamic 导出，插件可解析到）。
// 设置页里会如实写明这一点，不假装它是沙箱。
//
// 纪律：钩子在 UI 线程且已持锁 → 绝不 LOCKLV、绝不做文件 I/O。
//       .conf 读写全部在本文件的 worker 线程完成。

#include "lios.h"
#include "lios_perm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PERM_DIR "/mnt/UDISK/liosdata/perm"
#define PERM_DIR_BASE "/mnt/UDISK/liosdata"
#define PERM_KEYS 4
#define MAX_ROWS 32

static const char *kPermKeys[PERM_KEYS] = {"storage", "camera", "network", "bg"};
static const char *kPermNames[PERM_KEYS] = {"读写存储", "相机与视频", "网络", "后台运行"};

typedef struct
{
    char id[64];
    int on[PERM_KEYS];
} perm_rec_t;

// ---------------- 共享（全部由 s_mtx 保护）
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static perm_rec_t *s_recs = NULL;
static int s_rec_n = 0;
static bool s_loaded = false;
static bool s_load_req = false;
static bool s_save_req = false;
static bool s_busy = false;
static char s_save_id[64];
static int s_save_on[PERM_KEYS];
static char s_status[128];

// ---------------- UI
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_rows[MAX_ROWS] = {0};
static lv_obj_t *s_row_lbl[MAX_ROWS] = {0};
static int s_row_app[MAX_ROWS] = {0}; // 行 -> 应用下标（应用列表视图用）
static int s_view = 0;                // 0=应用列表 1=权限项
static char s_cur_id[64] = "";
static bool s_built = false;   // 本页是否已重建过（每次建页重置）
static int s_focus_want = 0;   // 重建后希望聚焦的行（避免切换开关后被拽回第一行）

static void ui_rebuild(void);
static void row_click_cb(lv_event_t *e);

// ---------------- 权限查询（导出给插件/其它模块）
static int find_rec_locked(const char *id)
{
    for (int i = 0; i < s_rec_n; i++)
        if (strcmp(s_recs[i].id, id) == 0)
            return i;
    return -1;
}

bool lios_perm_allowed(const char *id, const char *key)
{
    if (id == NULL || key == NULL)
        return false;
    pthread_mutex_lock(&s_mtx);
    int idx = find_rec_locked(id);
    bool ok = false;
    if (idx >= 0)
        for (int i = 0; i < PERM_KEYS; i++)
            if (strcmp(kPermKeys[i], key) == 0)
            {
                ok = s_recs[idx].on[i] != 0;
                break;
            }
    pthread_mutex_unlock(&s_mtx);
    return ok;
}

// ---------------- 文件读写（只在 worker 里调用）
static void perm_path(const char *id, char *out, size_t n)
{
    snprintf(out, n, "%s/%s.conf", PERM_DIR, id);
}

static void load_one(const char *id, perm_rec_t *rec)
{
    snprintf(rec->id, sizeof(rec->id), "%s", id);
    for (int i = 0; i < PERM_KEYS; i++)
        rec->on[i] = 1; // 默认全开（第一版不做拦截，默认开更顺手；用户可逐项关）

    char p[256];
    perm_path(id, p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (f == NULL)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = 0;
        int v = atoi(eq + 1);
        for (int i = 0; i < PERM_KEYS; i++)
            if (strcmp(line, kPermKeys[i]) == 0)
                rec->on[i] = v ? 1 : 0;
    }
    fclose(f);
}

static bool save_one(const char *id, const int *on)
{
    mkdir(PERM_DIR_BASE, 0755);
    mkdir(PERM_DIR, 0755);
    char p[256], tmp[300];
    perm_path(id, p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return false;
    for (int i = 0; i < PERM_KEYS; i++)
        fprintf(f, "%s=%d\n", kPermKeys[i], on[i] ? 1 : 0);
    fclose(f);
    return rename(tmp, p) == 0;
}

// ---------------- worker
static void worker_load_all(void)
{
    mkdir(PERM_DIR_BASE, 0755);
    mkdir(PERM_DIR, 0755);

    int total = lios_apps_total();
    perm_rec_t *arr = (perm_rec_t *)calloc(total > 0 ? (size_t)total : 1, sizeof(perm_rec_t));
    int n = 0;
    if (arr != NULL)
        for (int i = 0; i < total; i++)
        {
            const lios_app_def_t *a = lios_apps_get(i);
            if (a == NULL || a->id == NULL)
                continue;
            load_one(a->id, &arr[n]);
            n++;
        }

    pthread_mutex_lock(&s_mtx);
    if (s_recs != NULL)
        free(s_recs);
    s_recs = arr;
    s_rec_n = n;
    s_loaded = true;
    snprintf(s_status, sizeof(s_status), "已加载 %d 个应用的权限", n);
    pthread_mutex_unlock(&s_mtx);
}

static void worker_save(const char *id, const int *on)
{
    bool ok = save_one(id, on);
    pthread_mutex_lock(&s_mtx);
    snprintf(s_status, sizeof(s_status), ok ? "已保存" : "保存失败");
    s_busy = false;
    pthread_mutex_unlock(&s_mtx);
}

static void *perm_worker(void *)
{
    for (;;)
    {
        int job = 0;
        char id[64];
        int on[PERM_KEYS];

        pthread_mutex_lock(&s_mtx);
        if (s_load_req && !s_busy)
        {
            s_load_req = false;
            job = 1;
        }
        else if (s_save_req && !s_busy)
        {
            s_save_req = false;
            s_busy = true;
            job = 2;
            snprintf(id, sizeof(id), "%s", s_save_id);
            memcpy(on, s_save_on, sizeof(on));
        }
        pthread_mutex_unlock(&s_mtx);

        if (job == 1)
            worker_load_all();
        else if (job == 2)
            worker_save(id, on);
        else
            usleep(30 * 1000);
    }
    return NULL;
}

static void perm_worker_start(void)
{
    static bool started = false;
    if (started)
        return;
    started = true;
    pthread_t t;
    if (pthread_create(&t, NULL, perm_worker, NULL) == 0)
        pthread_detach(t);
    else
        lios_log("权限管理：worker 创建失败");

    pthread_mutex_lock(&s_mtx);
    s_load_req = true;
    pthread_mutex_unlock(&s_mtx);
}

// ---------------- 行池 / 渲染
static void row_pool(void)
{
    if (s_rows[0] != NULL)
        return;
    for (int i = 0; i < MAX_ROWS; i++)
    {
        lv_obj_t *r = lv_btn_create(s_list);
        lv_obj_set_size(r, 316, 22);
        lv_obj_set_pos(r, 0, i * 22);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_radius(r, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(r, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(r, lv_color_hex(0x69C0FF), LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *l = lv_label_create(r);
        lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, row_click_cb, LV_EVENT_CLICKED, NULL);
        s_rows[i] = r;
        s_row_lbl[i] = l;
    }
}

static void ui_rebuild(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list))
        return;
    row_pool();
    int n = 0;

    if (s_view == 0)
    {
        int total = lios_apps_total();
        pthread_mutex_lock(&s_mtx);
        for (int i = 0; i < total && n < MAX_ROWS; i++)
        {
            const lios_app_def_t *a = lios_apps_get(i);
            if (a == NULL)
                continue;
            int cnt = 0;
            int ri = find_rec_locked(a->id);
            if (ri >= 0)
                for (int k = 0; k < PERM_KEYS; k++)
                    cnt += s_recs[ri].on[k] ? 1 : 0;
            char t[180];
            snprintf(t, sizeof(t), "%s  %s   开 %d/%d", a->name,
                     lios_apps_is_plugin(i) ? "[插件]" : "[内置]", cnt, PERM_KEYS);
            lv_label_set_text(s_row_lbl[n], t);
            s_row_app[n] = i;
            n++;
        }
        pthread_mutex_unlock(&s_mtx);
        if (s_title != NULL && lv_obj_is_valid(s_title))
            lv_label_set_text(s_title, "权限管理（确认=进入）");
        if (s_hint != NULL && lv_obj_is_valid(s_hint))
            lv_label_set_text(s_hint, "第一版只记录与展示，不做强制拦截");
    }
    else
    {
        pthread_mutex_lock(&s_mtx);
        int ri = find_rec_locked(s_cur_id);
        for (int k = 0; k < PERM_KEYS && n < MAX_ROWS; k++)
        {
            char t[160];
            snprintf(t, sizeof(t), "%s    %s", kPermNames[k],
                     (ri >= 0 && s_recs[ri].on[k]) ? "开" : "关");
            lv_label_set_text(s_row_lbl[n], t);
            s_row_app[n] = k;
            n++;
        }
        pthread_mutex_unlock(&s_mtx);
        if (s_title != NULL && lv_obj_is_valid(s_title))
            lv_label_set_text(s_title, s_cur_id);
        if (s_hint != NULL && lv_obj_is_valid(s_hint))
            lv_label_set_text(s_hint, "确认=开/关   菜单=返回应用列表");
    }

    for (int i = 0; i < MAX_ROWS; i++)
    {
        if (s_rows[i] == NULL || !lv_obj_is_valid(s_rows[i]))
            continue;
        if (i < n)
        {
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
        }
        else
        {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
        }
    }
    if (n > 0)
    {
        int want = s_focus_want;
        if (want < 0 || want >= n)
            want = 0;
        lv_group_focus_obj(s_rows[want]);
    }
    s_focus_want = 0;
}

static void row_click_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (row < 0 || row >= MAX_ROWS)
        return;

    if (s_view == 0)
    {
        const lios_app_def_t *a = lios_apps_get(s_row_app[row]);
        if (a == NULL || a->id == NULL)
            return;
        snprintf(s_cur_id, sizeof(s_cur_id), "%s", a->id);
        s_view = 1;
        ui_rebuild();
    }
    else
    {
        int k = s_row_app[row];
        if (k < 0 || k >= PERM_KEYS)
            return;
        pthread_mutex_lock(&s_mtx);
        int ri = find_rec_locked(s_cur_id);
        if (ri >= 0)
        {
            s_recs[ri].on[k] = s_recs[ri].on[k] ? 0 : 1;
            if (!s_save_req && !s_busy)
            {
                s_save_req = true;
                snprintf(s_save_id, sizeof(s_save_id), "%s", s_cur_id);
                memcpy(s_save_on, s_recs[ri].on, sizeof(s_save_on));
            }
        }
        pthread_mutex_unlock(&s_mtx);
        s_focus_want = row; // 切换后仍停在原来这一行
        ui_rebuild();
    }
}

// ---------------- 钩子
static bool page_key(int key)
{
    if (key == LIOS_K_MENU)
    {
        if (s_view == 1)
        {
            s_view = 0;
            s_focus_want = 0;
            ui_rebuild();
            return true;
        }
        return false;
    }
    if (key == LIOS_K_ENTER)
    {
        static uint32_t last = 0;
        uint32_t now = lv_tick_get();
        if (now - last < 200)
            return true;
        last = now;
        return true; // 真正的点击由 LVGL 对焦点对象派发
    }
    return key == LIOS_K_REC;
}

static void page_tick(void)
{
    bool loaded = false;
    char st[128];
    pthread_mutex_lock(&s_mtx);
    loaded = s_loaded;
    snprintf(st, sizeof(st), "%s", s_status);
    pthread_mutex_unlock(&s_mtx);

    if (loaded && !s_built)
    {
        s_built = true;
        ui_rebuild();
    }
    if (st[0] != 0)
    {
        pthread_mutex_lock(&s_mtx);
        s_status[0] = 0;
        pthread_mutex_unlock(&s_mtx);
        if (s_hint != NULL && lv_obj_is_valid(s_hint))
            lv_label_set_text(s_hint, st);
    }
}

static void page_destroy(void)
{
    s_list = NULL;
    s_hint = NULL;
    s_title = NULL;
    for (int i = 0; i < MAX_ROWS; i++)
    {
        s_rows[i] = NULL;
        s_row_lbl[i] = NULL;
    }
    s_view = 0;
    s_cur_id[0] = 0;
    s_built = false;
}

static const lios_page_hooks_t s_hooks = {page_key, page_tick, page_destroy};

void page_perm_create(lv_obj_t *parent)
{
    perm_worker_start();

    // 每次进页面都重新读一遍权限文件 + 重置重建标志（否则第二次进来会一直显示"正在读取"）
    pthread_mutex_lock(&s_mtx);
    s_loaded = false;
    s_built = false;
    s_load_req = true;
    pthread_mutex_unlock(&s_mtx);

    s_view = 0;
    s_title = lv_label_create(parent);
    lv_obj_set_style_text_font(s_title, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_title, "权限管理（确认=进入）");
    lv_obj_set_pos(s_title, 4, 4);

    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, 316, 240 - 24 - 16);
    lv_obj_set_pos(s_list, 2, 24);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x7D8A97), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_hint, "正在读取权限记录…");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    lios_page_set_hooks(&s_hooks);
    lios_log("权限管理：页面已建");
}
