#ifndef UI_TRACE_H
#define UI_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

// 分步日志：写 /mnt/UDISK/ui.log，闪退后看最后一行即可定位
void ui_trace(const char *fmt, ...);
void ui_trace_reset(void);

#ifdef __cplusplus
}
#endif

#endif
