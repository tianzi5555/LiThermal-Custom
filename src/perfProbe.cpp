/**
 * @file perfProbe.cpp
 *
 * 临时性能计时探针的实现（诊断用，可整体删除）。
 *
 * 设计要点：
 *  - 只写 /tmp/perf.log（tmpfs 内存盘），不写 /mnt/UDISK，避免日志把 SD 卡写入拖慢。
 *  - 每 1000ms 汇总输出一次：每段的 平均/最大/次数。日志开销 ≈ 每秒 2 行。
 *  - 计时用 clock_gettime(CLOCK_MONOTONIC)（不阻塞、不受系统时间调整影响）。
 *  - 累加用 32 位整数（µs），在 32 位 ARM 上读写是原子的，避免 double 撕裂；
 *    汇总线程用 __sync 原子地「读走并清零」，最坏情况丢一个样本，对定位没有影响。
 */

#include "perfProbe.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#define PERF_LOG_PATH "/tmp/perf.log"
#define PERF_LOG_PATH_ALT "/dev/shm/perf.log" /* /tmp 不可写时的内存盘备选 */
#define PERF_REPORT_MS 1000

typedef struct
{
    unsigned int count;
    unsigned int sum_us;
    unsigned int max_us;
} perf_slot_t;

static perf_slot_t s_slots[PERF_SLOT_MAX];
static unsigned int s_counters[PERF_C_MAX];
static unsigned int s_gauges[PERF_G_MAX];

static void (*s_1s_hook)(void) = NULL;
static FILE *s_log = NULL;
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_start_us = 0;
static int s_reporter_started = 0;

uint32_t perf_probe_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL);
}

/* 单行写日志：整行一次 fwrite + fflush，避免多线程交错 */
static void perf_write_line(const char *line)
{
    pthread_mutex_lock(&s_log_mutex);
    if (s_log != NULL)
    {
        fputs(line, s_log);
        fputc('\n', s_log);
        fflush(s_log);
    }
    pthread_mutex_unlock(&s_log_mutex);
}

void perf_probe_note(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    if (s_log == NULL)
        return; /* init 之前调用就丢弃，避免自己造出 /tmp 写入 */

    n = snprintf(buf, sizeof(buf), "[%8.3fs] ", (double)(perf_probe_now_us() - s_start_us) / 1000000.0);
    if (n < 0)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    perf_write_line(buf);
}

void perf_probe_add(int slot, uint32_t us)
{
    perf_slot_t *s;
    if (slot < 0 || slot >= PERF_SLOT_MAX)
        return;
    s = &s_slots[slot];
    __sync_fetch_and_add(&s->count, 1u);
    __sync_fetch_and_add(&s->sum_us, us);
    if (us > s->max_us)
        s->max_us = us; /* 只是取最大值，非原子也无所谓 */
}

void perf_probe_count(int counter, unsigned int delta)
{
    if (counter < 0 || counter >= PERF_C_MAX)
        return;
    __sync_fetch_and_add(&s_counters[counter], delta);
}

void perf_probe_gauge(int gauge, unsigned int value)
{
    if (gauge < 0 || gauge >= PERF_G_MAX)
        return;
    s_gauges[gauge] = value;
}

void perf_probe_set_1s_hook(void (*fn)(void))
{
    s_1s_hook = fn;
}

static void *perf_reporter(void *arg)
{
    struct timespec ts;
    (void)arg;

    ts.tv_sec = PERF_REPORT_MS / 1000;
    ts.tv_nsec = (long)(PERF_REPORT_MS % 1000) * 1000000L;

    while (1)
    {
        unsigned int cnt[PERF_SLOT_MAX], sum[PERF_SLOT_MAX], mx[PERF_SLOT_MAX];
        unsigned int ev[PERF_C_MAX];
        unsigned int hook_us = 0;
        char line[768];
        double avg[PERF_SLOT_MAX];
        int i;
        int n;

        nanosleep(&ts, NULL);

        for (i = 0; i < PERF_SLOT_MAX; i++)
        {
            cnt[i] = __sync_lock_test_and_set(&s_slots[i].count, 0u);
            sum[i] = __sync_lock_test_and_set(&s_slots[i].sum_us, 0u);
            mx[i] = __sync_lock_test_and_set(&s_slots[i].max_us, 0u);
            avg[i] = (cnt[i] > 0) ? ((double)sum[i] / (double)cnt[i] / 1000.0) : 0.0;
        }
        for (i = 0; i < PERF_C_MAX; i++)
            ev[i] = __sync_lock_test_and_set(&s_counters[i], 0u);

        /* 每秒一次的 vsync 探测（在汇总线程里做，最多阻塞 1 帧，不影响渲染线程） */
        if (s_1s_hook != NULL)
        {
            uint32_t t0 = perf_probe_now_us();
            s_1s_hook();
            hook_us = perf_probe_now_us() - t0;
        }

        /* 第 1 行：各段耗时（ms），格式按需求固定，后面追加 max */
        n = snprintf(line, sizeof(line),
                     "perf: t=%.1fs loop_avg=%.1f loop_max=%.1f getTemp_avg=%.1f getTemp_max=%.1f "
                     "snap_avg=%.1f snap_max=%.1f getframe_avg=%.1f getframe_max=%.1f "
                     "proc_avg=%.2f proc_max=%.2f lv_avg=%.2f lv_max=%.2f "
                     "flush_avg=%.2f flush_max=%.2f flushPan_avg=%.2f flushPan_max=%.2f "
                     "period_avg=%.1f hal_avg=%.2f",
                     (double)(perf_probe_now_us() - s_start_us) / 1000000.0,
                     avg[PERF_LOOP], (double)mx[PERF_LOOP] / 1000.0,
                     avg[PERF_GETTEMP], (double)mx[PERF_GETTEMP] / 1000.0,
                     avg[PERF_SNAP], (double)mx[PERF_SNAP] / 1000.0,
                     avg[PERF_GETFRAME], (double)mx[PERF_GETFRAME] / 1000.0,
                     avg[PERF_PROC], (double)mx[PERF_PROC] / 1000.0,
                     avg[PERF_LV], (double)mx[PERF_LV] / 1000.0,
                     avg[PERF_FLUSH], (double)mx[PERF_FLUSH] / 1000.0,
                     avg[PERF_FLUSH_PAN], (double)mx[PERF_FLUSH_PAN] / 1000.0,
                     avg[PERF_LOOP_PERIOD], avg[PERF_HAL]);
        if (n > 0)
            perf_write_line(line);

        /* 第 2 行：次数（能直接看出哪一段被高频调用 / 哪条显示路径没走） */
        n = snprintf(line, sizeof(line),
                     "perf_n: loop=%u getTemp=%u snap=%u getframe=%u proc=%u lv=%u flush=%u flush_last=%u "
                     "pan=%u g2d_blit=%u hal=%u center=%u vsync_ret=%u vsync_errno=%u vsync_hook_us=%u "
                     "yoffset_rb=%u",
                     cnt[PERF_LOOP], cnt[PERF_GETTEMP], cnt[PERF_SNAP], cnt[PERF_GETFRAME], cnt[PERF_PROC],
                     cnt[PERF_LV], cnt[PERF_FLUSH], ev[PERF_C_FLUSH_LAST], ev[PERF_C_PAN], ev[PERF_C_G2D_BLIT],
                     cnt[PERF_HAL], cnt[PERF_CENTER], s_gauges[PERF_G_VSYNC_RET], s_gauges[PERF_G_VSYNC_ERRNO],
                     hook_us, s_gauges[PERF_G_YOFFSET_READBACK]);
        if (n > 0)
            perf_write_line(line);
    }
    return NULL;
}

void perf_probe_init(void)
{
    pthread_t th;
    const char *used = NULL;

    if (s_start_us == 0)
        s_start_us = perf_probe_now_us();

    if (s_log == NULL)
    {
        s_log = fopen(PERF_LOG_PATH, "w"); /* 每次启动截断 */
        used = PERF_LOG_PATH;
        if (s_log == NULL)
        {
            s_log = fopen(PERF_LOG_PATH_ALT, "w"); /* /tmp 不可写时退到另一个内存盘 */
            used = PERF_LOG_PATH_ALT;
        }
    }

    perf_probe_note("perf_probe init: pid=%d log=%s", (int)getpid(),
                    (s_log != NULL) ? used : "(FAILED: /tmp 和 /dev/shm 都写不了)");
    if (s_log == NULL)
        return;

    if (!s_reporter_started)
    {
        s_reporter_started = 1;
        if (pthread_create(&th, NULL, perf_reporter, NULL) == 0)
            pthread_detach(th);
        else
            perf_probe_note("perf_probe: reporter thread create FAILED");
    }
}
