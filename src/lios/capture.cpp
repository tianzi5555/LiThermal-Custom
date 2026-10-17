// LiOS 截图 / 录屏（加进桌面程序，非独立应用）
//   截图：电源键 + 录像键（短按）→ /mnt/UDISK/Screenshots/SHOT_*.bmp
//   录屏：电源键 + 菜单键（短按）→ 开始/停止 → /mnt/UDISK/Screenshots/REC_*.avi
// 帧来源：LVGL 的绘制缓冲 s_fb_buf（ARGB8888，320x240）
// 录屏：libavcodec MJPEG 编码 + libavformat AVI 封装，10fps

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CAP_W 320
#define CAP_H 240
#define CAP_FPS 10

// 由 main.cpp 提供
extern lv_color_t *lios_fb_buf(void);
extern uint32_t lios_fb_w(void);
extern uint32_t lios_fb_h(void);
extern void lios_lock_display(void);   // LOCKLV
extern void lios_unlock_display(void); // UNLOCKLV
extern void lios_log(const char *fmt, ...);

static const char *kDir = "/mnt/UDISK/Screenshots";

static void ensure_dir(void)
{
    mkdir(kDir, 0755);
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

// ---------------- 截图：ARGB8888 -> 24bit BGR BMP（bottom-up）
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
    // BMP 是 bottom-up
    for (int32_t y = h - 1; y >= 0; y--)
    {
        const lv_color_t *src = buf + (size_t)y * w;
        size_t o = 0;
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t c = src[x].full; // ARGB8888
            row[o++] = (unsigned char)((c >> 0) & 0xFF);  // B
            row[o++] = (unsigned char)((c >> 8) & 0xFF);  // G
            row[o++] = (unsigned char)((c >> 16) & 0xFF); // R
        }
        fwrite(row, 1, row_size, f);
        if (pad)
        {
            static const unsigned char z[3] = {0, 0, 0};
            fwrite(z, 1, pad, f);
        }
    }
    fclose(f);
    lios_log("截图已保存 %s", path);
}

void lios_capture_screenshot(void)
{
    ensure_dir();
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
        free(copy);
    }
    else
        lios_unlock_display();
}

// ---------------- 录屏：MJPEG -> AVI
static pthread_t s_rec_thread;
static volatile bool s_rec_run = false;
static volatile bool s_rec_done = false;
static char s_rec_path[160];

static void *rec_thread(void *)
{
    const char *path = s_rec_path;
    unlink(path);

    AVFormatContext *fmt = NULL;
    avformat_alloc_output_context2(&fmt, NULL, "avi", path);
    if (fmt == NULL)
    {
        lios_log("录屏：无法创建输出上下文");
        s_rec_done = true;
        return NULL;
    }
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (codec == NULL)
    {
        lios_log("录屏：没有 MJPEG 编码器");
        s_rec_done = true;
        return NULL;
    }
    AVStream *st = avformat_new_stream(fmt, NULL);
    if (st == NULL)
    {
        s_rec_done = true;
        return NULL;
    }
    AVCodecContext *cc = avcodec_alloc_context3(codec);
    if (cc == NULL)
    {
        s_rec_done = true;
        return NULL;
    }
    cc->width = CAP_W;
    cc->height = CAP_H;
    cc->time_base = (AVRational){1, CAP_FPS};
    cc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    cc->codec_id = AV_CODEC_ID_MJPEG;
    if (avcodec_open2(cc, codec, NULL) < 0)
    {
        lios_log("录屏：编码器打开失败");
        s_rec_done = true;
        return NULL;
    }
    avcodec_parameters_from_context(st->codecpar, cc);
    st->time_base = cc->time_base;

    if (avio_open(&fmt->pb, path, AVIO_FLAG_WRITE) < 0)
    {
        lios_log("录屏：打不开输出文件");
        s_rec_done = true;
        return NULL;
    }
    avformat_write_header(fmt, NULL);

    struct SwsContext *sws = sws_getContext(CAP_W, CAP_H, AV_PIX_FMT_BGRA, CAP_W, CAP_H,
                                            AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (sws == NULL)
    {
        s_rec_done = true;
        return NULL;
    }

    AVFrame *frame = av_frame_alloc();
    AVFrame *yuv = av_frame_alloc();
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    if (frame == NULL || yuv == NULL)
    {
        s_rec_done = true;
        return NULL;
    }
    av_frame_make_writable(yuv);
    yuv->format = AV_PIX_FMT_YUVJ420P;
    yuv->width = CAP_W;
    yuv->height = CAP_H;
    if (av_frame_get_buffer(yuv, 32) < 0)
    {
        s_rec_done = true;
        return NULL;
    }

    uint32_t w = lios_fb_w(), h = lios_fb_h();
    lv_color_t *tmp = (lv_color_t *)malloc((size_t)w * h * sizeof(lv_color_t));
    if (tmp == NULL)
    {
        s_rec_done = true;
        return NULL;
    }

    int64_t frame_no = 0;
    while (s_rec_run)
    {
        // 抓帧（锁内拷贝）
        lios_lock_display();
        lv_color_t *buf = lios_fb_buf();
        if (buf != NULL)
            memcpy(tmp, buf, (size_t)w * h * sizeof(lv_color_t));
        lios_unlock_display();

        uint8_t *src[4] = {(uint8_t *)tmp, NULL, NULL, NULL};
        int src_linesize[4] = {(int)w * 4, 0, 0, 0};
        sws_scale(sws, src, src_linesize, 0, h, yuv->data, yuv->linesize);
        yuv->pts = frame_no++;

        int got = 0;
        if (avcodec_encode_video2(cc, &pkt, yuv, &got) == 0 && got)
        {
            pkt.stream_index = st->index;
            av_interleaved_write_frame(fmt, &pkt);
            av_packet_unref(&pkt);
        }
        usleep(1000000 / CAP_FPS);
    }
    // 收尾：刷剩余帧
    int got = 0;
    if (avcodec_encode_video2(cc, &pkt, NULL, &got) == 0 && got)
    {
        pkt.stream_index = st->index;
        av_interleaved_write_frame(fmt, &pkt);
        av_packet_unref(&pkt);
    }
    av_write_trailer(fmt);

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_frame_free(&yuv);
    avcodec_free_context(&cc);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    free(tmp);
    lios_log("录屏已保存 %s（%lld 帧）", path, (long long)frame_no);
    s_rec_done = true;
    return NULL;
}

// 电源+菜单：开始/停止录屏
void lios_capture_rec_toggle(void)
{
    ensure_dir();
    if (s_rec_run)
    {
        s_rec_run = false; // 停止信号，线程自己收尾
        lios_log("录屏：请求停止");
    }
    else
    {
        char ts[32];
        timestamp(ts, sizeof(ts));
        snprintf(s_rec_path, sizeof(s_rec_path), "%s/REC_%s.avi", kDir, ts);
        s_rec_run = true;
        s_rec_done = false;
        if (pthread_create(&s_rec_thread, NULL, rec_thread, NULL) == 0)
        {
            pthread_detach(s_rec_thread);
            lios_log("录屏：开始 -> %s", s_rec_path);
        }
        else
        {
            s_rec_run = false;
            s_rec_done = true;
        }
    }
}
