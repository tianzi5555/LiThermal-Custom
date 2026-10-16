#include "videoPlayer.h"
#include "perfProbe.h" // 临时性能探针（诊断用，可删）
#include <sys/time.h>
#include <semaphore.h>
#include <string.h>
#include "videoCodec.h"
VideoPlayer videoPlayer;
uint8_t IR_frame_buffer[320 * 240 * 4];

// 数码变焦鸟览图
#define ZOOM_PIP_W 96
#define ZOOM_PIP_H 72
static uint8_t IR_thumb_buffer[ZOOM_PIP_W * ZOOM_PIP_H * 4];
static lv_img_dsc_t img_ir_thumb;
static lv_obj_t *zoom_pip = NULL;      // 右下角鸟览图容器
static lv_obj_t *zoom_pip_img = NULL;  // 鸟览图图像
static lv_obj_t *zoom_pip_rect = NULL; // 鸟览图中的当前区域框
int g_last_crop[6] = {0, 0, 0, 0, 0, 0}; // cropX,cropY,cropW,cropH,srcW,srcH
// 鸟览图区域框上一次已经设置过的裁剪区域，用来挡掉每帧重复的 set_pos/set_size
static int s_pip_last_crop[6] = {-1, -1, -1, -1, -1, -1};

pthread_t thread_image_ref;
static sem_t sem_video;
#define CMD_NONE 0
#define CMD_CONNECT 1
#define CMD_PLAY 2
#define CMD_PAUSE 3
#define CMD_DISCONNECT 4

int thread_video_command = CMD_NONE;

static lv_img_dsc_t img_ir_frame;

// 以下 ui_zoom_pip_* 函数均需要在持有 lv_mutex 的情况下调用
static void ui_zoom_pip_create()
{
    if (zoom_pip != NULL && lv_obj_is_valid(zoom_pip))
        return;

    zoom_pip = lv_obj_create(lv_layer_top());
    lv_obj_set_size(zoom_pip, ZOOM_PIP_W, ZOOM_PIP_H);
    lv_obj_align(zoom_pip, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_clear_flag(zoom_pip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(zoom_pip, 0, 0);
    lv_obj_set_style_bg_color(zoom_pip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(zoom_pip, LV_OPA_70, 0);
    lv_obj_set_style_border_width(zoom_pip, 1, 0);
    lv_obj_set_style_border_color(zoom_pip, lv_color_white(), 0);
    lv_obj_set_style_radius(zoom_pip, 0, 0);

    img_ir_thumb.header.always_zero = 0;
    img_ir_thumb.header.w = ZOOM_PIP_W;
    img_ir_thumb.header.h = ZOOM_PIP_H;
    img_ir_thumb.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_ir_thumb.data = IR_thumb_buffer;
    img_ir_thumb.data_size = ZOOM_PIP_W * ZOOM_PIP_H * 4;

    zoom_pip_img = lv_img_create(zoom_pip);
    lv_obj_set_pos(zoom_pip_img, 0, 0);
    lv_img_set_src(zoom_pip_img, &img_ir_thumb);

    zoom_pip_rect = lv_obj_create(zoom_pip);
    lv_obj_set_style_bg_opa(zoom_pip_rect, LV_OPA_0, 0);
    lv_obj_set_style_border_width(zoom_pip_rect, 1, 0);
    lv_obj_set_style_border_color(zoom_pip_rect, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_radius(zoom_pip_rect, 0, 0);
    lv_obj_clear_flag(zoom_pip_rect, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(zoom_pip, LV_OBJ_FLAG_HIDDEN);

    // 容器重建时清掉区域框的“已应用”缓存，保证下面第一帧一定会重新摆一次
    for (int i = 0; i < 6; i++)
        s_pip_last_crop[i] = -1;
}

static void ui_zoom_pip_update(int cropX, int cropY, int cropW, int cropH, int srcW, int srcH)
{
    if (zoom_pip_rect == NULL || srcW <= 0 || srcH <= 0)
        return;

    // 裁剪区域只有用户转动旋钮改变倍数时才会变，其余每一帧都是同一组值。
    // 而 lv_obj_set_pos/lv_obj_set_size 在 LVGL 8.4 里即使值没变也会
    // invalidate 对象（还会顺带 mark_layout_as_dirty），这个函数是每帧、
    // 而且是持 lv_mutex 调用的，所以要把重复设置挡掉，别白白拉长临界区。
    // 显示效果不变：位置/尺寸和原来每帧设成同样的值完全一致。
    if (s_pip_last_crop[0] == cropX && s_pip_last_crop[1] == cropY &&
        s_pip_last_crop[2] == cropW && s_pip_last_crop[3] == cropH &&
        s_pip_last_crop[4] == srcW && s_pip_last_crop[5] == srcH)
        return;
    s_pip_last_crop[0] = cropX;
    s_pip_last_crop[1] = cropY;
    s_pip_last_crop[2] = cropW;
    s_pip_last_crop[3] = cropH;
    s_pip_last_crop[4] = srcW;
    s_pip_last_crop[5] = srcH;

    int rx = cropX * ZOOM_PIP_W / srcW;
    int ry = cropY * ZOOM_PIP_H / srcH;
    int rw = cropW * ZOOM_PIP_W / srcW;
    int rh = cropH * ZOOM_PIP_H / srcH;
    if (rw < 3) rw = 3;
    if (rh < 3) rh = 3;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > ZOOM_PIP_W) rw = ZOOM_PIP_W - rx;
    if (ry + rh > ZOOM_PIP_H) rh = ZOOM_PIP_H - ry;

    lv_obj_set_pos(zoom_pip_rect, rx, ry);
    lv_obj_set_size(zoom_pip_rect, rw, rh);
}

void ui_zoom_pip_check_visibility()
{
    if (zoom_pip == NULL)
        return;

    bool show = (globalSettings.digitalZoom > 100) && (current_mode == MODE_MAINPAGE);

    if (show)
    {
        lv_obj_clear_flag(zoom_pip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(zoom_pip);
    }
    else
    {
        lv_obj_add_flag(zoom_pip, LV_OBJ_FLAG_HIDDEN);
    }
}

// 计算亮度映射表：先做自动对比度（2%~98% 百分位拉伸），再做手动对比度
static void build_luma_map(uint8_t map[256], const uint8_t *src, int srcW, int srcH, int srcStride)
{
    unsigned hist[256];
    memset(hist, 0, sizeof(hist));

    int sample = 1;
    if (srcW * srcH > 150000)
        sample = 2;

    int n = 0;
    for (int y = 0; y < srcH; y += sample)
    {
        const uint8_t *row = src + y * srcStride;
        for (int x = 0; x < srcW; x += sample)
        {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];
            int l = (r + g + b) / 3;
            hist[l]++;
            n++;
        }
    }

    int low = 0;
    int high = 255;

    if (globalSettings.autoContrast && n > 0)
    {
        unsigned acc = 0;
        unsigned low_target = (unsigned)(n * 2ULL / 100ULL);
        unsigned high_target = (unsigned)(n * 98ULL / 100ULL);
        for (int i = 0; i < 256; i++)
        {
            acc += hist[i];
            if (acc >= low_target) { low = i; break; }
        }
        acc = 0;
        for (int i = 255; i >= 0; i--)
        {
            acc += hist[i];
            if (acc >= (unsigned)(n - high_target)) { high = i; break; }
        }
        if (high - low < 8)
        {
            low = 0;
            high = 255;
        }
    }

    float contrast_factor = globalSettings.contrast / 50.0f;
    for (int i = 0; i < 256; i++)
    {
        int v = i;
        if (globalSettings.autoContrast)
        {
            if (high > low)
                v = (v - low) * 255 / (high - low);
            else
                v = 0;
        }
        if (globalSettings.contrast != 50)
        {
            v = (int)((v - 128) * contrast_factor + 128);
        }
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        map[i] = (uint8_t)v;
    }
}

// 最近邻缩放的索引表
//
// 原来 320x240 的内层循环每帧都要做 320*240 次整数除法（cropX + x*cropW/320），
// 鸟览图再加 96*72 次；ARMv7 上整数除法要几十个周期，光这一项每帧就要好几毫秒，
// 而这些索引只跟「裁剪区域 + 源尺寸」有关，也就是只在用户转动旋钮改变倍数时才变。
// 所以把行列索引缓存成表，每帧只查表。表里的值就是用原来那条表达式算出来的，
// 结果逐位一致，画面不会有任何变化。
static int s_main_col_index[320];
static int s_main_row_index[240];
static int s_thumb_col_index[ZOOM_PIP_W];
static int s_thumb_row_index[ZOOM_PIP_H];
static int s_tbl_cropX = -1, s_tbl_cropW = -1, s_tbl_srcW = -1;
static int s_tbl_cropY = -1, s_tbl_cropH = -1, s_tbl_srcH = -1;

static void build_index_tables(int cropX, int cropY, int cropW, int cropH, int srcW, int srcH)
{
    if (s_tbl_cropX != cropX || s_tbl_cropW != cropW || s_tbl_srcW != srcW)
    {
        for (int x = 0; x < 320; x++)
        {
            int v = cropX + (x * cropW) / 320;
            if (v >= srcW) v = srcW - 1;
            s_main_col_index[x] = v;
        }
        for (int x = 0; x < ZOOM_PIP_W; x++)
        {
            int v = (x * srcW) / ZOOM_PIP_W;
            if (v >= srcW) v = srcW - 1;
            s_thumb_col_index[x] = v;
        }
        s_tbl_cropX = cropX;
        s_tbl_cropW = cropW;
        s_tbl_srcW = srcW;
    }

    if (s_tbl_cropY != cropY || s_tbl_cropH != cropH || s_tbl_srcH != srcH)
    {
        for (int y = 0; y < 240; y++)
        {
            int v = cropY + (y * cropH) / 240;
            if (v >= srcH) v = srcH - 1;
            s_main_row_index[y] = v;
        }
        for (int y = 0; y < ZOOM_PIP_H; y++)
        {
            int v = (y * srcH) / ZOOM_PIP_H;
            if (v >= srcH) v = srcH - 1;
            s_thumb_row_index[y] = v;
        }
        s_tbl_cropY = cropY;
        s_tbl_cropH = cropH;
        s_tbl_srcH = srcH;
    }
}

static void process_frame_to_buffers(AVFrame *frame)
{
    PERF_SCOPE(PERF_PROC); // 临时探针：裁剪 + 缩放（CPU 逐像素）全程
    int srcW = frame->width;
    int srcH = frame->height;
    int srcStride = frame->linesize[0];
    const uint8_t *src = frame->data[0];

    if (src == NULL || srcW <= 0 || srcH <= 0 || srcStride <= 0)
    {
        memset(IR_frame_buffer, 0, sizeof(IR_frame_buffer));
        memset(IR_thumb_buffer, 0, sizeof(IR_thumb_buffer));
        return;
    }

    int zoom = (int)globalSettings.digitalZoom;
    if (zoom < 100) zoom = 100;
    if (zoom > 400) zoom = 400;

    int cropW = (int)(srcW * 100.0f / zoom + 0.5f);
    int cropH = (int)(srcH * 100.0f / zoom + 0.5f);
    if (cropW < 1) cropW = 1;
    if (cropH < 1) cropH = 1;
    if (cropW > srcW) cropW = srcW;
    if (cropH > srcH) cropH = srcH;
    int cropX = (srcW - cropW) / 2;
    int cropY = (srcH - cropH) / 2;

    uint8_t luma_map[256];
    build_luma_map(luma_map, src, srcW, srcH, srcStride);

    // 预计算 RGB 缩放表，避免每个像素做除法
    uint32_t rgb_scale[256];
    for (int i = 1; i < 256; i++)
        rgb_scale[i] = ((uint32_t)luma_map[i] << 16) / (uint32_t)i;
    rgb_scale[0] = 0;

    // 预计算最近邻缩放的行列索引（只在倍数变化时重算，见上面的说明）
    build_index_tables(cropX, cropY, cropW, cropH, srcW, srcH);

    // 1) 主画面：裁剪区域最近邻缩放到 320x240
    uint8_t *dst = IR_frame_buffer;
    for (int y = 0; y < 240; y++)
    {
        const uint8_t *src_row = src + s_main_row_index[y] * srcStride;
        uint8_t *dst_row = dst + y * 320 * 4;
        for (int x = 0; x < 320; x++)
        {
            const uint8_t *s = src_row + s_main_col_index[x] * 4;
            int b = s[0];
            int g = s[1];
            int r = s[2];
            int a = s[3];
            int l = (r + g + b) / 3;
            if (l > 0)
            {
                uint32_t k = rgb_scale[l];
                dst_row[0] = (uint8_t)((b * k) >> 16);
                dst_row[1] = (uint8_t)((g * k) >> 16);
                dst_row[2] = (uint8_t)((r * k) >> 16);
            }
            else
            {
                uint8_t m = luma_map[0];
                dst_row[0] = m;
                dst_row[1] = m;
                dst_row[2] = m;
            }
            dst_row[3] = (uint8_t)a;
            dst_row += 4;
        }
    }

    // 2) 鸟览图：全图缩略到 96x72
    uint8_t *thumb = IR_thumb_buffer;
    for (int y = 0; y < ZOOM_PIP_H; y++)
    {
        const uint8_t *src_row = src + s_thumb_row_index[y] * srcStride;
        uint8_t *thumb_row = thumb + y * ZOOM_PIP_W * 4;
        for (int x = 0; x < ZOOM_PIP_W; x++)
        {
            const uint8_t *s = src_row + s_thumb_col_index[x] * 4;
            int b = s[0];
            int g = s[1];
            int r = s[2];
            int a = s[3];
            int l = (r + g + b) / 3;
            if (l > 0)
            {
                uint32_t k = rgb_scale[l];
                thumb_row[0] = (uint8_t)((b * k) >> 16);
                thumb_row[1] = (uint8_t)((g * k) >> 16);
                thumb_row[2] = (uint8_t)((r * k) >> 16);
            }
            else
            {
                uint8_t m = luma_map[0];
                thumb_row[0] = m;
                thumb_row[1] = m;
                thumb_row[2] = m;
            }
            thumb_row[3] = (uint8_t)a;
            thumb_row += 4;
        }
    }

    g_last_crop[0] = cropX;
    g_last_crop[1] = cropY;
    g_last_crop[2] = cropW;
    g_last_crop[3] = cropH;
    g_last_crop[4] = srcW;
    g_last_crop[5] = srcH;
}

static void createImage(bool isResume)
{
    (void)isResume;
    videoPlayer.img_obj = lv_img_create(lv_scr_act());
    lv_obj_set_size(videoPlayer.img_obj, 320, 240);
    img_ir_frame.header.always_zero = 0;
    img_ir_frame.header.w = 320;
    img_ir_frame.header.h = 240;
    img_ir_frame.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_ir_frame.data = IR_frame_buffer;
    img_ir_frame.data_size = 320 * 240 * 4;
    lv_img_set_src(videoPlayer.img_obj, &img_ir_frame);
    lv_obj_align(videoPlayer.img_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_opa(videoPlayer.img_obj, 0, 0);

    ui_zoom_pip_create();
}
static void destroyImage()
{
    if (lv_obj_is_valid(videoPlayer.img_obj))
    {
        lv_my_anim_fall_down(videoPlayer.img_obj);
        videoPlayer.img_obj = NULL;
    }
}
uint64_t getTimeStampUS()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

#define STATE_IDLE 1
#define STATE_PLAYING 2
#define STATE_PAUSED 3

static bool connected = false;
static int current_state = STATE_IDLE;

void *thread_refresh_image(void *)
{
    static int err_count = 0;
    sem_wait(&sem_video);
    usleep(500 * 1000);
    while (1)
    {
        switch (thread_video_command)
        {
        case CMD_CONNECT:
        {
        retry:
            printf("Connceting to RTSP...\n");
            if (codec_openStream(VIDEO_STREAM_URL))
            {
                current_state = STATE_PLAYING;
                connected = true;
                cameraUtils.connected = true;
                LOCKLV();
                createImage(false);
                UNLOCKLV();
                printf("Stream open success\n");
            }
            else
            {
                printf("Stream open fail, retrying...\n");
                codec_closeEverything();
                sleep(1);
                goto retry;
            }
        }
        break;
        case CMD_PLAY:
        {
            if (connected)
            {
                current_state = STATE_PLAYING;
            }
        }
        break;
        case CMD_PAUSE:
        {
            if (connected)
            {
                current_state = STATE_PAUSED;
            }
        }
        break;
        case CMD_DISCONNECT:
        {
            if (connected)
            {
                current_state = STATE_IDLE;
                connected = false;
                LOCKLV();
                destroyImage();
                UNLOCKLV();
                codec_closeEverything();
            }
        }
        break;
        default:
            break;
        }
        thread_video_command = CMD_NONE;
        switch (current_state)
        {
        case STATE_IDLE:
            sem_wait(&sem_video);
            break;
        case STATE_PLAYING:
        {
            /* --- perf probe: codec_getFrame 全程（RTSP 收包 + 解码） --- */
            uint32_t perf_gf_t0 = perf_probe_now_us();
            auto frame = codec_getFrame();
            perf_probe_add(PERF_GETFRAME, perf_probe_now_us() - perf_gf_t0);
            if (frame == NULL)
            {
                printf("empty\n");
                ++err_count;
                if (err_count > 10)
                {
                    current_state = STATE_IDLE;
                    connected = false;
                    codec_closeEverything();
                }
                continue;
            }
            err_count = 0;
            process_frame_to_buffers(frame);
            codec_writeProcessedFrame(IR_frame_buffer);
            av_frame_free(&frame);
            LOCKLV();
            if (lv_obj_is_valid(videoPlayer.img_obj))
            {
                if (lv_obj_get_style_opa(videoPlayer.img_obj, 0) == 0)
                {
                    lv_obj_fade_in(videoPlayer.img_obj, 500, 0);
                }
                lv_obj_invalidate(videoPlayer.img_obj);
            }
            if (zoom_pip != NULL && !lv_obj_has_flag(zoom_pip, LV_OBJ_FLAG_HIDDEN))
            {
                if (zoom_pip_rect != NULL && globalSettings.digitalZoom > 100)
                    ui_zoom_pip_update(g_last_crop[0], g_last_crop[1], g_last_crop[2], g_last_crop[3], g_last_crop[4], g_last_crop[5]);
                lv_obj_invalidate(zoom_pip_img);
                lv_obj_move_foreground(zoom_pip);
            }
            UNLOCKLV();
            usleep(20000);
            sem_trywait(&sem_video);
        }
        break;
        case STATE_PAUSED:
            sem_wait(&sem_video);
            break;
        default:
            break;
        }
    }
    return NULL;
}

void VideoPlayer::init()
{
    thread_image_ref = CMD_NONE;
    sem_init(&sem_video, 0, 0);
    avformat_network_init();
    pthread_create(&thread_image_ref, NULL, thread_refresh_image, NULL);
}

void VideoPlayer::connect()
{
    thread_video_command = CMD_CONNECT;
    sem_post(&sem_video);
}

void VideoPlayer::play()
{
    thread_video_command = CMD_PLAY;
    sem_post(&sem_video);
}

void VideoPlayer::disconnect()
{
    thread_video_command = CMD_DISCONNECT;
    sem_post(&sem_video);
}
