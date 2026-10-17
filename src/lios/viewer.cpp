// LiOS 文件查看器（图片 / 视频 / 文本）—— 被文件管理器调用
//
// 契约：
//   * viewer_open() 只建 LVGL 对象 + 起 worker 线程，**绝不做文件 I/O、绝不解码**
//   * viewer_tick()/viewer_key()/viewer_wheel() 都在 UI 线程、调用者已持锁时执行；
//     里面只取 worker 的结果并更新 LVGL（绝不 LOCKLV、绝不阻塞、绝不读文件）
//   * 解码/读文件全在 worker 线程；退出时 viewer_close() 会 join 线程并释放
//     解码器 + 缓冲（117MB 内存，别泄）
//
// 解码方案：ffmpeg（avformat/avcodec/swscale）解第一帧/顺序帧 → 缩放到适应 320x240
// （保持长宽比、黑色居中补边）→ BGRA 缓冲 → lv_canvas 直接显示（LV_COLOR_DEPTH=32 时
// lv_color_t 的内存顺序就是 B,G,R,A，正好是 AV_PIX_FMT_BGRA，零转换）。
//
// 视频：双缓冲 + 丢帧。worker 只写「UI 当前没在显示的那块」，写好才发布；
//       UI 每帧最多取一次。解码慢的时候旧帧直接丢，绝不卡 UI。

#include "lios.h"
#include "viewer.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define VW 320 // 屏幕宽
#define VH 240 // 屏幕高
#define TEXT_LIMIT (64 * 1024)
#define PATH_MAX_LEN 480

// ---------------------------------------------------------------- 状态
enum
{
    ST_IDLE = 0,
    ST_WORKING,
    ST_READY, // 文本已就绪
    ST_ERR
};

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_canvas = NULL; // 图片/视频
static lv_obj_t *s_body = NULL;   // 文本滚动容器
static lv_obj_t *s_lbl = NULL;    // 文本 / 错误文字
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_hint = NULL;

static int s_kind = VIEW_OTHER;
static char s_path[PATH_MAX_LEN];

static uint8_t *s_buf[2] = {NULL, NULL}; // BGRA 320x240
static char *s_text = NULL;              // 文本缓冲（worker malloc）
static volatile int s_state = ST_IDLE;
static char s_err[128];
static volatile bool s_eof = false;

static pthread_t s_thr;
static bool s_thr_run = false;
static volatile bool s_stop = false;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_show = 0;       // UI 正在显示的缓冲下标
static int s_ready_idx = -1; // worker 刚填好的缓冲
static bool s_ready = false;
static bool s_told_eof = false; // 「已结束」只提示一次
static bool s_err_shown = false; // 错误页只建一次

// ---------------------------------------------------------------- 工具
static void set_hint_text(const char *s)
{
    if (s_hint != NULL && lv_obj_is_valid(s_hint))
        lv_label_set_text(s_hint, s);
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// 可被 s_stop 打断的睡眠（worker 线程里用；分片睡，退出最多迟 20ms）
static void sleep_us_stop(uint64_t us)
{
    while (us > 0 && !s_stop)
    {
        uint64_t chunk = (us > 20000ULL) ? 20000ULL : us;
        usleep((useconds_t)chunk);
        us -= chunk;
    }
}

// ---------------------------------------------------------------- 类型判断
static bool ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return false;
    return strcasecmp(dot, ext) == 0;
}

int viewer_kind_of(const char *name, unsigned long long size)
{
    if (name == NULL)
        return VIEW_OTHER;
    if (ext_is(name, ".jpg") || ext_is(name, ".jpeg") || ext_is(name, ".png") || ext_is(name, ".bmp"))
        return VIEW_IMAGE;
    if (ext_is(name, ".mp4") || ext_is(name, ".avi") || ext_is(name, ".mkv") ||
        ext_is(name, ".mov") || ext_is(name, ".flv"))
        return VIEW_VIDEO;
    if (ext_is(name, ".txt") || ext_is(name, ".log") || ext_is(name, ".conf") || ext_is(name, ".sh") ||
        ext_is(name, ".json") || ext_is(name, ".md") || ext_is(name, ".csv"))
        return VIEW_TEXT;
    if (size < TEXT_LIMIT)
        return VIEW_TEXT; // 小文件当文本试试
    return VIEW_OTHER;
}

bool viewer_is_open(void)
{
    return s_page != NULL;
}

// ---------------------------------------------------------------- 解码（worker 线程）
typedef struct
{
    AVFormatContext *fmt;
    AVCodecContext *dec;
    struct SwsContext *sws;
    AVPacket *pkt;
    AVFrame *frm;
    int vst;
    int dw, dh, x0, y0;
} dec_t;

static void media_close(dec_t *d)
{
    if (d->sws != NULL)
        sws_freeContext(d->sws);
    if (d->dec != NULL)
        avcodec_free_context(&d->dec);
    if (d->fmt != NULL)
        avformat_close_input(&d->fmt);
    if (d->pkt != NULL)
        av_packet_free(&d->pkt);
    if (d->frm != NULL)
        av_frame_free(&d->frm);
    memset(d, 0, sizeof(*d));
}

// 打开容器 + 找第一条视频流 + 开解码器。失败写 err。
static bool media_open(dec_t *d, const char *path, char *err, size_t errn)
{
    memset(d, 0, sizeof(*d));
    d->vst = -1;

    int r = avformat_open_input(&d->fmt, path, NULL, NULL);
    if (r < 0)
    {
        // 有些图片（单帧 png/bmp/jpg）用 image2 更稳，再试一次
        // 注意：这份 ffmpeg（libavformat 58.76）的 ff_const59 是空的，
        // av_find_input_format / avformat_open_input 都是非 const 指针，别写 const。
        AVInputFormat *ifmt = av_find_input_format("image2");
        if (ifmt != NULL)
            r = avformat_open_input(&d->fmt, path, ifmt, NULL);
    }
    if (r < 0 || d->fmt == NULL)
    {
        snprintf(err, errn, "打不开这个文件");
        return false;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0)
    {
        snprintf(err, errn, "读不出文件内容");
        return false;
    }

    // 这份 ffmpeg 的 av_find_best_stream 要的是 AVCodec **（非 const），照它写
    AVCodec *codec = NULL;
    d->vst = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (d->vst < 0 || codec == NULL)
    {
        snprintf(err, errn, "里面没有画面");
        return false;
    }

    d->dec = avcodec_alloc_context3(codec);
    if (d->dec == NULL)
    {
        snprintf(err, errn, "内存不足");
        return false;
    }
    if (avcodec_parameters_to_context(d->dec, d->fmt->streams[d->vst]->codecpar) < 0)
    {
        snprintf(err, errn, "解码器参数不对");
        return false;
    }
    d->dec->thread_count = 1; // 单核设备，多线程解码反而更慢
    if (avcodec_open2(d->dec, codec, NULL) < 0)
    {
        snprintf(err, errn, "没有对应的解码器");
        return false;
    }

    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    if (d->pkt == NULL || d->frm == NULL)
    {
        snprintf(err, errn, "内存不足");
        return false;
    }
    return true;
}

// 按第一帧的实际尺寸建缩放器（保持长宽比，居中，黑边）
static bool ensure_sws(dec_t *d, const AVFrame *f)
{
    if (d->sws != NULL)
        return true;
    int w = f->width, h = f->height;
    if (w <= 0 || h <= 0)
        return false;
    double ar = (double)w / (double)h;
    int dw = VW, dh = (int)((double)VW / ar + 0.5);
    if (dh > VH)
    {
        dh = VH;
        dw = (int)((double)VH * ar + 0.5);
    }
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;
    d->dw = dw;
    d->dh = dh;
    d->x0 = (VW - dw) / 2;
    d->y0 = (VH - dh) / 2;
    d->sws = sws_getContext(w, h, (enum AVPixelFormat)f->format, dw, dh, AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, NULL, NULL, NULL);
    return d->sws != NULL;
}

static void scale_into(dec_t *d, const AVFrame *f, uint8_t *buf)
{
    uint8_t *dst[4] = {buf + (size_t)(d->y0 * VW + d->x0) * 4u, NULL, NULL, NULL};
    int dstst[4] = {VW * 4, 0, 0, 0};
    sws_scale(d->sws, f->data, f->linesize, 0, f->height, dst, dstst);
}

// 取一块「UI 没在显示」的缓冲；返回 -1 = UI 还没取走上一帧，本帧丢掉
static int back_buffer(void)
{
    int w;
    pthread_mutex_lock(&s_mtx);
    w = s_show ^ 1;
    bool busy = s_ready;
    pthread_mutex_unlock(&s_mtx);
    return busy ? -1 : w;
}

static void publish(int idx)
{
    pthread_mutex_lock(&s_mtx);
    s_ready_idx = idx;
    s_ready = true;
    pthread_mutex_unlock(&s_mtx);
}

static void set_error(const char *msg)
{
    pthread_mutex_lock(&s_mtx);
    snprintf(s_err, sizeof(s_err), "%s", msg);
    s_state = ST_ERR;
    pthread_mutex_unlock(&s_mtx);
}

static double stream_fps(AVFormatContext *fmt, int vst)
{
    double fps = 0;
    AVRational fr = fmt->streams[vst]->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = fmt->streams[vst]->r_frame_rate;
    if (fr.den > 0)
        fps = (double)fr.num / (double)fr.den;
    if (fps < 1.0 || fps > 60.0)
        fps = 20.0; // 片源帧率不靠谱就用 20fps
    return fps;
}

// 图片/视频共用的解码线程
static void *decode_worker(void *)
{
    dec_t d;
    char err[128];
    if (!media_open(&d, s_path, err, sizeof(err)))
    {
        media_close(&d);
        set_error(err);
        return NULL;
    }

    double fps = stream_fps(d.fmt, d.vst);
    uint64_t min_interval = (uint64_t)(1000000.0 / fps);
    if (min_interval < 10000ULL)
        min_interval = 10000ULL; // 最高 100fps，别空转
    uint64_t last_pub = now_us();
    bool done = false;
    bool published_any = false;

    while (!s_stop && !done)
    {
        int r = av_read_frame(d.fmt, d.pkt);
        if (r < 0)
            break; // EOF
        if (d.pkt->stream_index != d.vst)
        {
            av_packet_unref(d.pkt);
            continue;
        }
        if (avcodec_send_packet(d.dec, d.pkt) < 0)
        {
            av_packet_unref(d.pkt);
            continue;
        }
        av_packet_unref(d.pkt);

        while (!s_stop)
        {
            if (avcodec_receive_frame(d.dec, d.frm) != 0)
                break;
            int w = back_buffer();
            if (w >= 0 && ensure_sws(&d, d.frm))
            {
                scale_into(&d, d.frm, s_buf[w]);
                publish(w);
                published_any = true;
            }
            av_frame_unref(d.frm);

            if (s_kind == VIEW_IMAGE)
            {
                done = true; // 图片只要第一帧
                break;
            }
            // 限速到片源帧率（解码比播放快时不要烧 CPU）
            uint64_t dt = now_us() - last_pub;
            if (dt < min_interval)
                sleep_us_stop(min_interval - dt);
            last_pub = now_us();
        }
    }

    // 视频收尾：把解码器里剩的帧吐出来（图片不走这里）
    if (!s_stop && s_kind == VIEW_VIDEO)
    {
        if (avcodec_send_packet(d.dec, NULL) >= 0)
        {
            while (!s_stop && avcodec_receive_frame(d.dec, d.frm) == 0)
            {
                int w = back_buffer();
                if (w >= 0 && ensure_sws(&d, d.frm))
                {
                    scale_into(&d, d.frm, s_buf[w]);
                    publish(w);
                    published_any = true;
                }
                av_frame_unref(d.frm);
            }
        }
    }

    media_close(&d);

    if (!s_stop)
    {
        if (!published_any)
            set_error("解码失败（文件有问题）");
        else if (s_kind == VIEW_VIDEO)
            s_eof = true;
    }
    return NULL;
}

// 文本读取线程（≤64KB，超出截断；非法 UTF-8 / 控制字符清洗掉）
static void *text_worker(void *)
{
    FILE *f = fopen(s_path, "rb");
    if (f == NULL)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "打不开这个文件（%s）", strerror(errno));
        set_error(buf);
        return NULL;
    }
    size_t cap = TEXT_LIMIT;
    size_t rawcap = cap + 512;
    char *raw = (char *)malloc(rawcap);
    if (raw == NULL)
    {
        fclose(f);
        set_error("内存不足");
        return NULL;
    }
    size_t n = fread(raw, 1, cap + 1, f);
    bool trunc = (n > cap);
    if (trunc)
        n = cap;
    fclose(f);

    // 清洗：换行保留；其它控制字符 -> 空格；非法 UTF-8 -> '?'
    size_t out = 0;
    for (size_t i = 0; i < n;)
    {
        unsigned char c = (unsigned char)raw[i];
        if (c == '\n')
        {
            raw[out++] = '\n';
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f)
        {
            raw[out++] = ' ';
            i++;
            continue;
        }
        if (c < 0x80)
        {
            raw[out++] = (char)c;
            i++;
            continue;
        }
        int len = (c >= 0xF0) ? 4 : ((c >= 0xE0) ? 3 : ((c >= 0xC0) ? 2 : 0));
        bool ok = (len > 0) && (i + (size_t)len <= n);
        for (int k = 1; ok && k < len; k++)
            if (((unsigned char)raw[i + k] & 0xC0) != 0x80)
                ok = false;
        if (ok)
        {
            for (int k = 0; k < len; k++)
                raw[out++] = raw[i + k];
            i += (size_t)len;
        }
        else
        {
            raw[out++] = '?';
            i++;
        }
        if (out + 64 >= rawcap)
            break;
    }
    if (trunc)
    {
        const char *tail = "\n\n（文件太大，只显示前面 64KB）";
        size_t tl = strlen(tail);
        if (out + tl + 1 > rawcap)
            out = rawcap - tl - 1;
        memcpy(raw + out, tail, tl);
        out += tl;
    }
    raw[out] = 0;

    pthread_mutex_lock(&s_mtx);
    s_text = raw;
    s_state = ST_READY;
    pthread_mutex_unlock(&s_mtx);
    return NULL;
}

// ---------------------------------------------------------------- 退出清理
static void join_worker(void)
{
    if (!s_thr_run)
        return;
    s_stop = true;
    pthread_join(s_thr, NULL);
    s_thr_run = false;
}

void viewer_close(void)
{
    join_worker();

    // 先删控件（它们引用缓冲/文本），再释放内存
    if (s_page != NULL && lv_obj_is_valid(s_page))
        lv_obj_del(s_page);
    s_page = NULL;
    s_canvas = NULL;
    s_body = NULL;
    s_lbl = NULL;
    s_title = NULL;
    s_hint = NULL;

    if (s_buf[0] != NULL)
    {
        free(s_buf[0]);
        s_buf[0] = NULL;
    }
    if (s_buf[1] != NULL)
    {
        free(s_buf[1]);
        s_buf[1] = NULL;
    }
    if (s_text != NULL)
    {
        free(s_text);
        s_text = NULL;
    }

    pthread_mutex_lock(&s_mtx);
    s_ready = false;
    s_ready_idx = -1;
    s_show = 0;
    s_state = ST_IDLE;
    s_err[0] = 0;
    pthread_mutex_unlock(&s_mtx);
    s_eof = false;
    s_kind = VIEW_OTHER;

    lios_desktop_set_wheel_focus(true); // 滚轮还给 LVGL（文件管理器列表要用）
    lios_log("查看器：已关闭并释放解码器/缓冲");
}

// ---------------------------------------------------------------- 界面
static lv_obj_t *make_overlay_label(lv_obj_t *parent, const char *text, lv_align_t align, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(l, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(l, LV_OPA_60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(l, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(l, align, x, y);
    return l;
}

static const char *kind_name(int kind)
{
    switch (kind)
    {
    case VIEW_IMAGE:
        return "图片";
    case VIEW_VIDEO:
        return "视频";
    case VIEW_TEXT:
        return "文本";
    default:
        return "文件";
    }
}

lv_obj_t *viewer_open(lv_obj_t *parent, const char *path, int kind)
{
    if (s_page != NULL)
        viewer_close(); // 保险：不该发生

    snprintf(s_path, sizeof(s_path), "%s", path);
    s_kind = kind;
    s_stop = false;
    s_eof = false;
    s_told_eof = false;
    s_err_shown = false;
    s_state = ST_WORKING;
    s_err[0] = 0;
    pthread_mutex_lock(&s_mtx);
    s_ready = false;
    s_ready_idx = -1;
    s_show = 0;
    pthread_mutex_unlock(&s_mtx);

    // 全屏页面
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, VW, VH);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_page, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(s_page);

    char title[360];
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    snprintf(title, sizeof(title), "%s  %s", kind_name(kind), base);

    if (kind == VIEW_TEXT)
    {
        s_title = lv_label_create(s_page);
        lv_obj_set_width(s_title, 316);
        lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
        lv_label_set_text(s_title, title);
        lv_obj_set_style_text_font(s_title, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_title, lv_color_hex(0x9AD7FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(s_title, 3, 2);

        s_body = lv_obj_create(s_page);
        lv_obj_set_size(s_body, 316, 198);
        lv_obj_set_pos(s_body, 2, 18);
        lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_bg_opa(s_body, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_body, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_body, 2, LV_PART_MAIN | LV_STATE_DEFAULT);

        s_lbl = lv_label_create(s_body);
        lv_obj_set_width(s_lbl, 308);
        lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_lbl, &ui_font_cn12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_lbl, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(s_lbl, "正在读取…");

        s_hint = make_overlay_label(s_page, "菜单键 返回  滚轮 上下", LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    }
    else
    {
        // 图片/视频：BGRA 双缓冲 + lv_canvas（LV_COLOR_DEPTH=32 时内存顺序 = B,G,R,A）
        s_buf[0] = (uint8_t *)calloc(1, (size_t)VW * VH * 4);
        s_buf[1] = (uint8_t *)calloc(1, (size_t)VW * VH * 4);
        if (s_buf[0] == NULL || s_buf[1] == NULL)
        {
            if (s_buf[0] != NULL)
            {
                free(s_buf[0]);
                s_buf[0] = NULL;
            }
            if (s_buf[1] != NULL)
            {
                free(s_buf[1]);
                s_buf[1] = NULL;
            }
            // 没有缓冲就退化成错误页（绝不崩）
            s_lbl = lv_label_create(s_page);
            lv_label_set_text(s_lbl, "内存不足，看不了这个文件");
            lv_obj_set_style_text_font(s_lbl, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(s_lbl, lv_color_hex(0xE8EEF4), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_center(s_lbl);
            s_hint = make_overlay_label(s_page, "菜单键 返回", LV_ALIGN_BOTTOM_RIGHT, -2, -2);
            lios_desktop_set_wheel_focus(false);
            lios_log("查看器：缓冲分配失败 %s", path);
            return s_page;
        }

        s_canvas = lv_canvas_create(s_page);
        lv_obj_set_size(s_canvas, VW, VH);
        lv_obj_set_pos(s_canvas, 0, 0);
        lv_canvas_set_buffer(s_canvas, s_buf[0], VW, VH, LV_IMG_CF_TRUE_COLOR);

        s_title = make_overlay_label(s_page, title, LV_ALIGN_TOP_LEFT, 2, 2);
        s_hint = make_overlay_label(s_page, "菜单键 返回", LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    }

    lios_desktop_set_wheel_focus(false); // 滚轮自己收（文本滚动用）

    // 起 worker：读文件/解码全在那边，UI 只取结果
    void *(*fn)(void *) = (kind == VIEW_TEXT) ? text_worker : decode_worker;
    if (pthread_create(&s_thr, NULL, fn, NULL) == 0)
    {
        s_thr_run = true;
        lios_log("查看器：打开 %s（kind=%d）", path, kind);
    }
    else
    {
        s_state = ST_ERR;
        snprintf(s_err, sizeof(s_err), "线程创建失败");
        lios_log("查看器：worker 线程创建失败 %s", path);
    }
    return s_page;
}

// ---------------------------------------------------------------- UI 每帧
static void show_error_if_any(void)
{
    if (s_state != ST_ERR || s_err_shown)
        return;
    s_err_shown = true;
    if (s_canvas != NULL && lv_obj_is_valid(s_canvas))
    {
        lv_obj_del(s_canvas);
        s_canvas = NULL;
    }
    if (s_body != NULL && lv_obj_is_valid(s_body))
    {
        lv_obj_del(s_body);
        s_body = NULL;
        s_lbl = NULL;
    }
    s_lbl = lv_label_create(s_page);
    lv_obj_set_width(s_lbl, 300);
    lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_lbl, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl, lv_color_hex(0xFF8A8A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_lbl, LV_ALIGN_CENTER, 0, -6);
    char msg[220];
    snprintf(msg, sizeof(msg), "%s\n%s", (s_err[0] != 0) ? s_err : "打不开这个文件",
             (s_kind == VIEW_IMAGE) ? "图片" : ((s_kind == VIEW_VIDEO) ? "视频" : "文本"));
    lv_label_set_text(s_lbl, msg);
    set_hint_text("菜单键 返回");
    lios_log("查看器：打开失败 %s（%s）", s_path, s_err);
}

void viewer_tick(void)
{
    if (s_page == NULL)
        return;

    show_error_if_any();

    if (s_kind == VIEW_TEXT)
    {
        char *t = NULL;
        pthread_mutex_lock(&s_mtx);
        if (s_state == ST_READY && s_text != NULL)
        {
            t = s_text;
            s_text = NULL;
            s_state = ST_IDLE;
        }
        pthread_mutex_unlock(&s_mtx);
        if (t != NULL)
        {
            if (s_lbl != NULL && lv_obj_is_valid(s_lbl))
                lv_label_set_text_static(s_lbl, t); // 静态文本：不复制，零 LVGL 堆开销
            else if (s_lbl == NULL)
                s_text = t; // 不该发生
            lios_log("查看器：文本已显示 %s（%u 字节）", s_path, (unsigned)strlen(t));
        }
        return;
    }

    // 图片/视频：取 worker 刚填好的那一帧（双缓冲换页）
    int idx = -1;
    pthread_mutex_lock(&s_mtx);
    if (s_ready)
    {
        idx = s_ready_idx;
        s_show = idx;
        s_ready = false;
    }
    pthread_mutex_unlock(&s_mtx);
    if (idx >= 0 && s_canvas != NULL && lv_obj_is_valid(s_canvas) && s_buf[idx] != NULL)
    {
        lv_canvas_set_buffer(s_canvas, s_buf[idx], VW, VH, LV_IMG_CF_TRUE_COLOR);
        lv_obj_invalidate(s_canvas);
    }

    if (s_eof && !s_told_eof)
    {
        s_told_eof = true;
        set_hint_text("已结束  菜单键 返回");
        lios_log("查看器：%s 播放结束", s_path);
    }
}

// ---------------------------------------------------------------- 按键
bool viewer_key(int key)
{
    if (s_page == NULL)
        return false;
    if (key == LIOS_K_MENU)
    {
        viewer_close(); // 菜单键 = 退出回列表
        return true;
    }
    if (key == LIOS_K_ENTER || key == LIOS_K_REC)
        return true; // 看图/看片时什么都不做（别漏给桌面去关页面）
    return false;
}

void viewer_wheel(int diff)
{
    if (s_page == NULL || s_body == NULL || !lv_obj_is_valid(s_body))
        return;
    lv_obj_scroll_by(s_body, 0, (lv_coord_t)(-diff * 22), LV_ANIM_OFF);
}
