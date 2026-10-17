// LiOS 截图 / 录屏（加进桌面程序，非独立应用）
//   截图：电源键 + 录像键（短按）→ /mnt/UDISK/Screenshots/SHOT_*.bmp
//   录屏：电源键 + 菜单键（短按）→ 开始/停止 → /mnt/UDISK/Screenshots/REC_<时间>/*.bmp
// 帧来源：LVGL 的绘制缓冲 s_fb_buf（ARGB8888，320x240）
// 说明：工具链的 ffmpeg 是纯解码版（无编码器），所以录屏存 BMP 帧序列
//      （文件管理器能逐帧查看；要合成视频可在电脑上用 ffmpeg 批量转）

#include <lvgl.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CAP_FPS 8

extern lv_color_t *lios_fb_buf(void);
extern uint32_t lios_fb_w(void);
extern uint32_t lios_fb_h(void);
extern void lios_lock_display(void);
extern void lios_unlock_display(void);
extern "C" void lios_log(const char *fmt, ...); // lios_log 是 C 链接

static const char *kDir = "/mnt/UDISK/Screenshots";

static void ensure_dir(const char *p)
{
    mkdir(p, 0755);
}

static void timestamp(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm != NULL)
        snprintf(out, n, "%04d%02d%02d_%02d%02d%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        snprintf(out, n, "%ld", (long)t);
}

// ARGB8888 -> 24bit BGR BMP（bottom-up）
static void write_bmp(const lv_color_t *buf, uint32_t w, uint32_t h, const char *path)
{
    size_t row_size = w * 3;
    size_t pad = (4 - (row_size % 4)) % 4;
    size_t data_size = (row_size + pad) * h;
    unsigned char header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    uint32_t file_size = 54 + (uint32_t)data_size;
    memcpy(header + 2, &file_size, 4);
    header[10] = 54;
    header[14] = 40;
    int32_t w32 = (int32_t)w, h32 = (int32_t)h;
    memcpy(header + 18, &w32, 4);
    memcpy(header + 22, &h32, 4);
    header[26] = 1;
    header[28] = 24;

    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return;
    fwrite(header, 1, 54, f);
    unsigned char row[1024];
    for (int32_t y = h - 1; y >= 0; y--)
    {
        const lv_color_t *src = buf + (size_t)y * w;
        size_t o = 0;
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t c = src[x].full;
            row[o++] = (unsigned char)((c >> 0) & 0xFF);
            row[o++] = (unsigned char)((c >> 8) & 0xFF);
            row[o++] = (unsigned char)((c >> 16) & 0xFF);
        }
        fwrite(row, 1, row_size, f);
        if (pad)
        {
            static const unsigned char z[3] = {0, 0, 0};
            fwrite(z, 1, pad, f);
        }
    }
    fclose(f);
}

// ---------------- 截图
void lios_capture_screenshot(void)
{
    ensure_dir(kDir);
    lios_lock_display();
    uint32_t w = lios_fb_w(), h = lios_fb_h();
    lv_color_t *buf = lios_fb_buf();
    if (buf == NULL || w == 0 || h == 0)
    {
        lios_unlock_display();
        return;
    }
    lv_color_t *copy = (lv_color_t *)malloc((size_t)w * h * sizeof(lv_color_t));
    if (copy != NULL)
    {
        memcpy(copy, buf, (size_t)w * h * sizeof(lv_color_t));
        lios_unlock_display();
        char ts[32], path[160];
        timestamp(ts, sizeof(ts));
        snprintf(path, sizeof(path), "%s/SHOT_%s.bmp", kDir, ts);
        write_bmp(copy, w, h, path);
        lios_log("截图已保存 %s", path);
        free(copy);
    }
    else
        lios_unlock_display();
}

// ---------------- 录屏：BMP 帧序列
static pthread_t s_rec_thread;
static volatile bool s_rec_run = false;
static volatile bool s_rec_done = false;
static char s_rec_dir[160];

static void *rec_thread(void *)
{
    const char *dir = s_rec_dir;
    ensure_dir(dir);
    uint32_t w = lios_fb_w(), h = lios_fb_h();
    lv_color_t *tmp = (lv_color_t *)malloc((size_t)w * h * sizeof(lv_color_t));
    if (tmp == NULL)
    {
        s_rec_done = true;
        return NULL;
    }
    long frame_no = 0;
    while (s_rec_run)
    {
        lios_lock_display();
        lv_color_t *buf = lios_fb_buf();
        if (buf != NULL)
            memcpy(tmp, buf, (size_t)w * h * sizeof(lv_color_t));
        lios_unlock_display();

        char path[200];
        snprintf(path, sizeof(path), "%s/F%05ld.bmp", dir, frame_no);
        write_bmp(tmp, w, h, path);
        frame_no++;
        usleep(1000000 / CAP_FPS);
    }
    free(tmp);
    lios_log("录屏结束：%s 共 %ld 帧", dir, frame_no);
    s_rec_done = true;
    return NULL;
}

void lios_capture_rec_toggle(void)
{
    ensure_dir(kDir);
    if (s_rec_run)
    {
        s_rec_run = false;
        lios_log("录屏：请求停止");
    }
    else
    {
        char ts[32];
        timestamp(ts, sizeof(ts));
        snprintf(s_rec_dir, sizeof(s_rec_dir), "%s/REC_%s", kDir, ts);
        s_rec_run = true;
        s_rec_done = false;
        if (pthread_create(&s_rec_thread, NULL, rec_thread, NULL) == 0)
        {
            pthread_detach(s_rec_thread);
            lios_log("录屏：开始 -> %s", s_rec_dir);
        }
        else
        {
            s_rec_run = false;
            s_rec_done = true;
        }
    }
}
