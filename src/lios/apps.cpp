// LiOS 桌面 —— 应用清单（内置 + 自动发现的 .so 插件）
//
// 内置 5 个永远是：
//   0 相机（不是页面：桌面 fork+exec /mnt/UDISK/LiThermal）
//   1 文件管理器   2 设置   3 权限管理   4 后台
//
// 插件（用户往 SD 卡里丢的 .so）由后台线程自动发现：
//   * 扫描 /mnt/UDISK/apps/*.so 与 /mnt/UDISK/*.so（两个地方都认，apps/ 优先、同名去重）
//   * dlopen(RTLD_NOW) → 读 lios_app_info（兼容 lios_app_info_get()）
//     → 校验 lios_app_create / lios_app_destroy / lios_app_key 三个符号
//   * 校验失败 / ABI 不符 / id 重复 → 只写 /tmp/lios.log 并跳过，桌面绝不崩
//   * 排在内置应用之后，按 info.order 再按名字排序
//
// 纪律：扫描（opendir/dlopen）绝不在 UI 线程做 —— 这里是独立 worker 线程，
//       结果填好后才把 revision +1，UI 线程每帧比一下版本号再重建网格。

#include "lios.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// 插件 ABI（include/lios_app.h）—— 这里只用它的结构体与函数签名做校验，
// 不直接链插件里的符号（那要 dlopen 之后才拿得到）。
#include "lios_app.h"

#define APPS_DIR "/mnt/UDISK/apps"
#define ROOT_DIR "/mnt/UDISK"

#define MAX_PLUGINS 48
#define MAX_CAND 96
#define ID_MAX 32
#define NAME_MAX 48
#define ICON_MAX 12
#define PATH_MAX_LEN 480

// ---------------------------------------------------------------- 内置应用
// create 里只能建 LVGL 对象，**不能做文件 I/O**（会在 UI 线程/回调里卡住）。
// 需要读目录之类的事情一律交给 worker 线程（见 files.cpp / apps.cpp 底部）。
const lios_app_def_t LIOS_APPS[] = {
    // 相机：不是页面，桌面会 fork+exec /mnt/UDISK/LiThermal
    {"camera", "相机", "相", NULL, false},
    // 文件管理器：真页面（列表类，需要滚轮走焦点）
    {"files", "文件管理器", "文", page_files_create, true},
    // 下面三个还是占位页（第 3/4 批做成真页面）
    {"settings", "设置", "设", NULL, false},
    {"perm", "权限管理", "权", NULL, false},
    {"tasks", "后台", "后", NULL, false},
};

const int LIOS_APP_COUNT = (int)(sizeof(LIOS_APPS) / sizeof(LIOS_APPS[0]));

// ---------------------------------------------------------------- 插件槽
typedef struct
{
    lios_app_def_t def;      // def.id/name/icon 指向下面这几个缓冲
    char id[ID_MAX];
    char name[NAME_MAX];
    char icon[ICON_MAX];
    char path[PATH_MAX_LEN];
    int order;
    bool opened;             // 当前是否已 dlopen（页面打开中）
    void *handle;
    lios_plugin_ops_t ops;   // dlsym 结果（只在 opened 时有效）
} slot_t;

static slot_t s_plug[MAX_PLUGINS];
static int s_plug_n = 0;          // 已发布的插件数
static volatile int s_rev = 0;    // 清单版本：0 = 还没扫完
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_scan_started = false;

// ---------------------------------------------------------------- 内置访问
int lios_apps_total(void)
{
    int n;
    pthread_mutex_lock(&s_mtx);
    n = LIOS_APP_COUNT + s_plug_n;
    pthread_mutex_unlock(&s_mtx);
    return n;
}

int lios_apps_revision(void)
{
    int r;
    pthread_mutex_lock(&s_mtx);
    r = s_rev;
    pthread_mutex_unlock(&s_mtx);
    return r;
}

const lios_app_def_t *lios_apps_get(int i)
{
    if (i < 0)
        return NULL;
    if (i < LIOS_APP_COUNT)
        return &LIOS_APPS[i];
    int j = i - LIOS_APP_COUNT;
    pthread_mutex_lock(&s_mtx);
    const lios_app_def_t *d = (j < s_plug_n) ? &s_plug[j].def : NULL;
    pthread_mutex_unlock(&s_mtx);
    return d;
}

bool lios_apps_is_plugin(int i)
{
    return (i >= LIOS_APP_COUNT) && (i < lios_apps_total());
}

// ---------------------------------------------------------------- 候选收集
typedef struct
{
    char path[PATH_MAX_LEN];
    char base[NAME_MAX]; // 文件名（去重、排序用）
    bool from_apps_dir;
} cand_t;

static bool ends_with_so(const char *name)
{
    size_t n = strlen(name);
    return (n > 3) && (strcasecmp(name + n - 3, ".so") == 0);
}

static bool base_seen(const cand_t *c, int n, const char *base)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(c[i].base, base) == 0)
            return true;
    return false;
}

static int collect_dir(const char *dir, cand_t *out, int n, int cap, bool from_apps)
{
    DIR *d = opendir(dir);
    if (d == NULL)
    {
        lios_log("插件：%s 打不开（%s），跳过", dir, strerror(errno));
        return n;
    }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL)
    {
        if (!ends_with_so(e->d_name))
            continue;
        if (base_seen(out, n, e->d_name))
            continue; // 同名去重：apps/ 先扫，所以 apps/ 优先
        char full[PATH_MAX_LEN + NAME_MAX + 2];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (n >= cap)
        {
            lios_log("插件：候选太多（>%d），后面的忽略", cap);
            break;
        }
        snprintf(out[n].path, sizeof(out[n].path), "%s", full);
        snprintf(out[n].base, sizeof(out[n].base), "%s", e->d_name);
        out[n].from_apps_dir = from_apps;
        n++;
        found++;
    }
    closedir(d);
    lios_log("插件：扫描 %s -> %d 个 .so", dir, found);
    return n;
}

// ---------------------------------------------------------------- 排序
typedef struct
{
    int order;
    char name[NAME_MAX];
    char id[ID_MAX];
    char icon[ICON_MAX];
    char path[PATH_MAX_LEN];
} sortkey_t;

static int cmp_sortkey(const void *a, const void *b)
{
    const sortkey_t *x = (const sortkey_t *)a, *y = (const sortkey_t *)b;
    if (x->order != y->order)
        return x->order - y->order;
    return strcasecmp(x->name, y->name);
}

// ---------------------------------------------------------------- 单个 .so 的 ABI 校验
// 成功：填 id/name/icon/order 并返回 true；失败：写日志返回 false
static bool inspect(const char *path, char *id, char *name, char *icon, int *order)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL)
    {
        lios_log("插件：跳过 %s —— dlopen 失败：%s", path, dlerror());
        return false;
    }

    const lios_app_info_t *info = (const lios_app_info_t *)dlsym(h, "lios_app_info");
    if (info == NULL)
    {
        // 兼容另一种写法：函数返回结构体指针
        const lios_app_info_t *(*getter)(void) =
            (const lios_app_info_t *(*)(void))dlsym(h, "lios_app_info_get");
        if (getter != NULL)
            info = getter();
    }
    void *f_create = dlsym(h, "lios_app_create");
    void *f_destroy = dlsym(h, "lios_app_destroy");
    void *f_key = dlsym(h, "lios_app_key");

    bool ok = true;
    if (info == NULL)
    {
        lios_log("插件：跳过 %s —— 没有 lios_app_info", path);
        ok = false;
    }
    else if (f_create == NULL || f_destroy == NULL || f_key == NULL)
    {
        lios_log("插件：跳过 %s —— 缺符号（create=%p destroy=%p key=%p）", path, f_create, f_destroy, f_key);
        ok = false;
    }
    else if (info->id == NULL || info->name == NULL || info->icon == NULL ||
             info->id[0] == 0 || info->name[0] == 0 ||
             strlen(info->id) >= ID_MAX || strlen(info->name) >= NAME_MAX || strlen(info->icon) >= ICON_MAX)
    {
        lios_log("插件：跳过 %s —— ABI 不符（id/name/icon 不合法）", path);
        ok = false;
    }
    else if (info->order < -100000 || info->order > 100000 ||
             (info->need_fullscreen != 0 && info->need_fullscreen != 1))
    {
        lios_log("插件：跳过 %s —— ABI 不符（order/need_fullscreen 越界）", path);
        ok = false;
    }

    if (ok)
    {
        snprintf(id, ID_MAX, "%s", info->id);
        snprintf(name, NAME_MAX, "%s", info->name);
        snprintf(icon, ICON_MAX, "%s", info->icon);
        *order = info->order;
        lios_log("插件：OK %s -> id=%s name=%s order=%d", path, id, name, *order);
    }

    dlclose(h); // 这里只做校验，真正打开页面时再 dlopen（退出时 dlclose）
    return ok;
}

// ---------------------------------------------------------------- 扫描线程
static void *scan_thread(void *)
{
    cand_t cand[MAX_CAND];
    int n = 0;

    lios_log("插件：开始扫描 %s 与 %s", APPS_DIR, ROOT_DIR);
    n = collect_dir(APPS_DIR, cand, n, MAX_CAND, true);
    n = collect_dir(ROOT_DIR, cand, n, MAX_CAND, false);

    sortkey_t *keys = (sortkey_t *)calloc((size_t)(n > 0 ? n : 1), sizeof(sortkey_t));
    int kn = 0;
    int skipped = 0;

    for (int i = 0; i < n && kn < MAX_PLUGINS; i++)
    {
        char id[ID_MAX], name[NAME_MAX], icon[ICON_MAX];
        int order = 0;
        id[0] = name[0] = icon[0] = 0;
        if (!inspect(cand[i].path, id, name, icon, &order))
        {
            skipped++;
            continue;
        }
        // id 重复（不同 .so 用同一个 id）→ 后面的跳过
        bool dup = false;
        for (int k = 0; k < kn; k++)
            if (strcmp(keys[k].id, id) == 0)
            {
                dup = true;
                break;
            }
        if (dup)
        {
            lios_log("插件：跳过 %s —— id=%s 已存在", cand[i].path, id);
            skipped++;
            continue;
        }
        keys[kn].order = order;
        snprintf(keys[kn].name, sizeof(keys[kn].name), "%s", name);
        snprintf(keys[kn].id, sizeof(keys[kn].id), "%s", id);
        snprintf(keys[kn].icon, sizeof(keys[kn].icon), "%s", icon);
        snprintf(keys[kn].path, sizeof(keys[kn].path), "%s", cand[i].path);
        kn++;
    }

    if (kn > 1)
        qsort(keys, (size_t)kn, sizeof(sortkey_t), cmp_sortkey);

    // 发布：先把槽全部填好，最后一次性地放行（桌面看到 rev 变大才会去读槽）
    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < kn; i++)
    {
        slot_t *s = &s_plug[i];
        memset(s, 0, sizeof(*s));
        s->order = keys[i].order;
        snprintf(s->id, sizeof(s->id), "%s", keys[i].id);
        snprintf(s->name, sizeof(s->name), "%s", keys[i].name);
        snprintf(s->icon, sizeof(s->icon), "%s", keys[i].icon);
        snprintf(s->path, sizeof(s->path), "%s", keys[i].path);
        s->def.id = s->id;
        s->def.name = s->name;
        s->def.icon = s->icon;
        s->def.create = NULL;      // 插件页由 desktop 走 lios_apps_plugin_open 这条路
        s->def.wheel_focus = false;
        s->opened = false;
        s->handle = NULL;
    }
    s_plug_n = kn;
    s_rev++;
    pthread_mutex_unlock(&s_mtx);

    free(keys);
    lios_log("插件：扫描完成，收下 %d 个扩展应用（跳过 %d 个）", kn, skipped);
    return NULL;
}

void lios_apps_scan_start(void)
{
    if (s_scan_started)
        return;
    s_scan_started = true;
    pthread_t t;
    if (pthread_create(&t, NULL, scan_thread, NULL) == 0)
        pthread_detach(t);
    else
        lios_log("插件：扫描线程创建失败（只跑内置应用）");
}

// ---------------------------------------------------------------- 打开 / 关闭插件页
static slot_t *slot_of(int i)
{
    int j = i - LIOS_APP_COUNT;
    if (j < 0 || j >= MAX_PLUGINS)
        return NULL;
    return &s_plug[j];
}

const lios_plugin_ops_t *lios_apps_plugin_open(int i)
{
    slot_t *s = slot_of(i);
    if (s == NULL || s->id[0] == 0)
        return NULL;
    if (s->opened)
        return &s->ops;

    void *h = dlopen(s->path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL)
    {
        lios_log("插件：打开 %s 失败：%s", s->path, dlerror());
        return NULL;
    }

    memset(&s->ops, 0, sizeof(s->ops));
    s->ops.create = (lv_obj_t * (*)(lv_obj_t *)) dlsym(h, "lios_app_create");
    s->ops.destroy = (void (*)(void))dlsym(h, "lios_app_destroy");
    s->ops.key = (void (*)(int, int))dlsym(h, "lios_app_key");
    if (s->ops.create == NULL || s->ops.destroy == NULL || s->ops.key == NULL)
    {
        lios_log("插件：打开 %s 缺符号：%s", s->path, dlerror());
        dlclose(h);
        return NULL;
    }

    s->handle = h;
    s->opened = true;
    lios_log("插件：已打开 %s（%s）", s->id, s->path);
    return &s->ops;
}

void lios_apps_plugin_close(int i)
{
    slot_t *s = slot_of(i);
    if (s == NULL || !s->opened)
        return;
    s->opened = false;
    memset(&s->ops, 0, sizeof(s->ops));
    if (s->handle != NULL)
    {
        dlclose(s->handle);
        s->handle = NULL;
    }
    lios_log("插件：已关闭 %s（句柄已释放）", s->id);
}
