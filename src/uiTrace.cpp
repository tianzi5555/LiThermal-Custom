// uiTrace —— 极简分步日志，用于定位偶发闪退
//
// 写 /mnt/UDISK/ui.log（每次启动截断一次）。闪退后看最后一行就知道崩在哪一步。
// 只用 fopen/fprintf，不依赖任何非标准库；单行原子追加，开销可以忽略。

#include "uiTrace.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define UI_LOG_PATH "/mnt/UDISK/ui.log"
#define UI_LOG_MAX (256 * 1024)

static int s_trace_inited = 0;

void ui_trace_reset(void)
{
    FILE *f = fopen(UI_LOG_PATH, "w");
    if (f != NULL)
    {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        fprintf(f, "=== LiThermal uiTrace start %04d-%02d-%02d %02d:%02d:%02d ===\n",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        fclose(f);
    }
    s_trace_inited = 1;
}

void ui_trace(const char *fmt, ...)
{
    va_list ap;
    struct timeval tv;
    long size;
    FILE *f;

    if (!s_trace_inited)
    {
        FILE *f0 = fopen(UI_LOG_PATH, "a");
        if (f0 != NULL)
            fclose(f0);
        s_trace_inited = 1;
    }

    f = fopen(UI_LOG_PATH, "a");
    if (f == NULL)
        return;

    /* 太大就重开，避免把 UDISK 写满 */
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size > UI_LOG_MAX)
    {
        fclose(f);
        ui_trace_reset();
        f = fopen(UI_LOG_PATH, "a");
        if (f == NULL)
            return;
    }

    gettimeofday(&tv, NULL);
    fprintf(f, "[%ld.%03ld] ", (long)tv.tv_sec, (long)(tv.tv_usec / 1000));
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
