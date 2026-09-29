#include <stdio.h>
#include "videoCodec.h"
AVFormatContext *input_ctx = NULL;
AVFormatContext *output_ctx = NULL;
AVCodecContext *decoder_ctx = NULL;
AVStream *in_stream = NULL;
AVStream *out_stream = NULL;
struct SwsContext *sws_ctx = NULL;
AVPacket *packet = NULL;
AVFrame *frame = NULL;
int video_stream_index = -1;
bool packet_dumping = false;

// 处理画面录制（数码变焦/对比度之后）
// 采用 MPEG-4 Part 2 (AV_CODEC_ID_MPEG4) 真正重新编码为标准 MP4 视频流，
// 使录制文件在电脑上可直接双击播放，而不是仅把 MJPEG 帧塞进 MP4 容器。
#include <vector>
#include <stdarg.h>
#include <pthread.h>
static AVFormatContext *rec_ctx = NULL;
static AVStream *rec_stream = NULL;
static AVPacket *rec_pkt = NULL;
static AVCodecContext *rec_enc_ctx = NULL;   // MPEG-4 视频编码器上下文
static struct SwsContext *rec_sws = NULL;    // BGRA -> YUV420P 转换
static AVFrame *rec_yuv = NULL;              // 编码器输入帧
static uint8_t rec_bgra[320 * 240 * 4];      // 视频线程写入的最新一帧（BGRA）
static bool processed_recording = false;
static int64_t rec_pts = 0;
static int rec_write_count = 0;

// 编码在独立线程中进行：视频线程只拷贝最新一帧并通知，编码忙时直接丢帧，
// 避免 tiny_jpeg 编码阻塞视频解码/显示造成画面延迟。
static pthread_t rec_thread;
static pthread_mutex_t rec_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rec_frame_cond = PTHREAD_COND_INITIALIZER;
static bool rec_thread_running = false;
static bool rec_frame_pending = false;

static void rec_log(const char *fmt, ...)
{
    FILE *f = fopen("/tmp/lithermal_rec.log", "a");
    if (f == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
    fclose(f);
}


int openInputStream(const char *input_url)
{
    if (avformat_open_input(&input_ctx, input_url, NULL, NULL) < 0)
    {
        fprintf(stderr, "Could not open input stream.\n");
        return -1;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(input_ctx, NULL) < 0)
    {
        fprintf(stderr, "Could not find stream information.\n");
        return -1;
    }

    // Find the video stream
    for (int i = 0; i < input_ctx->nb_streams; i++)
    {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1)
    {
        fprintf(stderr, "Could not find a video stream.\n");
        return -1;
    }

    in_stream = input_ctx->streams[video_stream_index];
    return 0;
}

int openInputDecoder()
{
    AVCodec *decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);

    if (!decoder)
    {
        fprintf(stderr, "Failed to find MJPEG codec.\n");
        return -1;
    }

    decoder_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(decoder_ctx, in_stream->codecpar);

    if (avcodec_open2(decoder_ctx, decoder, NULL) < 0)
    {
        fprintf(stderr, "Failed to open decoder.\n");
        return -1;
    }
    return 0;
}

int openMisc()
{
    // 帧缓冲区
    frame = av_frame_alloc();
    if (!frame)
    {
        fprintf(stderr, "Could not allocate frame.\n");
        return -1;
    }
    packet = av_packet_alloc();
    sws_ctx = sws_getContext(decoder_ctx->width, decoder_ctx->height, decoder_ctx->pix_fmt,
                             decoder_ctx->width, decoder_ctx->height, AV_PIX_FMT_BGRA,
                             SWS_POINT, NULL, NULL, NULL);
}

bool codec_openStream(const char *url)
{
    if (openInputStream(url) < 0)
        return false;
    if (openInputDecoder() < 0)
        return false;
    if (openMisc() < 0)
        return false;
    return true;
}

void codec_closeEverything()
{
    if (input_ctx != NULL)
        avformat_close_input(&input_ctx);
    else if (output_ctx != NULL)
    {
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE))
        {
            av_write_frame(output_ctx, packet);
            av_write_trailer(output_ctx);
            avio_closep(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
        packet_dumping = false;
    }
    if (decoder_ctx != NULL)
        avcodec_free_context(&decoder_ctx);
    if (frame != NULL)
        av_frame_free(&frame);
    if (sws_ctx != NULL)
    {
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }
}

int openOutputFile(const char *output_file)
{
    if (in_stream == NULL || decoder_ctx == NULL)
    {
        fprintf(stderr, "No input stream.\n");
        return -1;
    }
    avformat_alloc_output_context2(&output_ctx, NULL, NULL, output_file);
    if (!output_ctx)
    {
        fprintf(stderr, "Could not create output context.\n");
        return -1;
    }
    out_stream = avformat_new_stream(output_ctx, NULL);
    if (!out_stream)
    {
        fprintf(stderr, "Failed to allocate output stream.\n");
        return -1;
    }
    out_stream->time_base = in_stream->time_base; // 设置输出流时基

    // avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx); // 如需重编码用这个
    avcodec_parameters_from_context(out_stream->codecpar, decoder_ctx); // 直接写入数据包

    ///////////////////////////////////////////////////////////////////////////////////////打开目标文件
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&output_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0)
        {
            fprintf(stderr, "Could not open output file.\n");
            return -1;
        }
    }
    ////写入输出头信息
    if (avformat_write_header(output_ctx, NULL) < 0)
    {
        fprintf(stderr, "Error occurred when writing header.\n");
        return -1;
    }

    return 0;
}

void codec_enablePacketDumping(bool en, const char *dump_target)
{
    if (packet_dumping != en)
    {
        packet_dumping = en;
        if (packet_dumping == true) // 打开
        {
            openOutputFile(dump_target);
        }
        else
        {
            if (output_ctx != NULL)
            {
                if (!(output_ctx->oformat->flags & AVFMT_NOFILE))
                {
                    av_write_frame(output_ctx, packet);
                    avio_closep(&output_ctx->pb);
                    av_write_trailer(output_ctx);
                }
                avformat_free_context(output_ctx);
            }
        }
    }
}

// 编码线程：把 rec_bgra 转成 YUV420P 并用 MPEG-4 编码器编码，写入 MP4 muxer。
// 注意：只有本线程会写 rec_ctx/rec_pkt/rec_enc_ctx/rec_yuv，start/stop 只在线程未运行时初始化/销毁它们。
static void rec_encode_and_write(AVFrame *in_frame)
{
    int ret = avcodec_send_frame(rec_enc_ctx, in_frame);
    if (ret < 0)
    {
        rec_log("write: send_frame failed ret=%d\n", ret);
        return;
    }
    while (ret >= 0)
    {
        ret = avcodec_receive_packet(rec_enc_ctx, rec_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
        {
            rec_log("write: receive_packet failed ret=%d\n", ret);
            break;
        }
        rec_pkt->stream_index = rec_stream->index;
        av_packet_rescale_ts(rec_pkt, rec_enc_ctx->time_base, rec_stream->time_base);
        int wret = av_interleaved_write_frame(rec_ctx, rec_pkt);
        if (rec_write_count < 5 || wret < 0)
            rec_log("write: count=%d size=%d ret=%d\n", rec_write_count, rec_pkt->size, wret);
        rec_write_count++;
        av_packet_unref(rec_pkt);
    }
}

static void *rec_thread_func(void *)
{
    for (;;)
    {
        pthread_mutex_lock(&rec_frame_mutex);
        while (!rec_frame_pending && rec_thread_running)
            pthread_cond_wait(&rec_frame_cond, &rec_frame_mutex);
        if (!rec_thread_running)
        {
            pthread_mutex_unlock(&rec_frame_mutex);
            break;
        }
        // pending 为 true，视频线程不会覆盖 rec_bgra，可以解锁后慢慢编码
        pthread_mutex_unlock(&rec_frame_mutex);

        // BGRA -> YUV420P
        const uint8_t *src_slices[1] = {rec_bgra};
        int src_stride[1] = {320 * 4};
        sws_scale(rec_sws, src_slices, src_stride, 0, 240,
                  rec_yuv->data, rec_yuv->linesize);
        rec_yuv->pts = rec_pts++;
        rec_encode_and_write(rec_yuv);

        pthread_mutex_lock(&rec_frame_mutex);
        rec_frame_pending = false;
        pthread_cond_broadcast(&rec_frame_cond);
        pthread_mutex_unlock(&rec_frame_mutex);
    }
    return NULL;
}

bool codec_startProcessedRecording(const char *filename, int width, int height)
{
    (void)width;
    (void)height;
    rec_log("start: %s\n", filename);
    if (processed_recording)
        codec_stopProcessedRecording();

    int ret = avformat_alloc_output_context2(&rec_ctx, NULL, NULL, filename);
    if (ret < 0)
    {
        rec_log("start: alloc_output_context2 failed ret=%d\n", ret);
        return false;
    }
    rec_log("start: oformat=%s flags=0x%x\n", rec_ctx->oformat->name ? rec_ctx->oformat->name : "null", rec_ctx->oformat->flags);

    rec_stream = avformat_new_stream(rec_ctx, NULL);
    if (rec_stream == NULL)
    {
        rec_log("start: new_stream failed\n");
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        return false;
    }

    // 用 MPEG-4 Part 2 编码器真正编码为标准 MP4 视频流
    AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (encoder == NULL)
    {
        rec_log("start: find MPEG4 encoder failed\n");
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }
    rec_enc_ctx = avcodec_alloc_context3(encoder);
    if (rec_enc_ctx == NULL)
    {
        rec_log("start: alloc encoder ctx failed\n");
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }
    rec_enc_ctx->width = 320;
    rec_enc_ctx->height = 240;
    rec_enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    rec_enc_ctx->time_base = (AVRational){1, 25};
    rec_enc_ctx->framerate = (AVRational){25, 1};
    rec_enc_ctx->gop_size = 25;
    rec_enc_ctx->max_b_frames = 0;
    rec_enc_ctx->bit_rate = 1500000; // 约 1.5Mbps，适合 320x240
    if (rec_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        rec_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(rec_enc_ctx, encoder, NULL);
    if (ret < 0)
    {
        rec_log("start: open encoder failed ret=%d\n", ret);
        avcodec_free_context(&rec_enc_ctx);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }

    rec_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    avcodec_parameters_from_context(rec_stream->codecpar, rec_enc_ctx);
    rec_stream->time_base = rec_enc_ctx->time_base;

    // BGRA -> YUV420P 转换器
    rec_sws = sws_getContext(320, 240, AV_PIX_FMT_BGRA,
                             320, 240, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (rec_sws == NULL)
    {
        rec_log("start: sws_getContext failed\n");
        avcodec_free_context(&rec_enc_ctx);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }
    rec_yuv = av_frame_alloc();
    if (rec_yuv == NULL)
    {
        rec_log("start: av_frame_alloc failed\n");
        sws_freeContext(rec_sws);
        rec_sws = NULL;
        avcodec_free_context(&rec_enc_ctx);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }
    rec_yuv->format = AV_PIX_FMT_YUV420P;
    rec_yuv->width = 320;
    rec_yuv->height = 240;
    if (av_frame_get_buffer(rec_yuv, 0) < 0)
    {
        rec_log("start: frame_get_buffer failed\n");
        av_frame_free(&rec_yuv);
        sws_freeContext(rec_sws);
        rec_sws = NULL;
        avcodec_free_context(&rec_enc_ctx);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }

    if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&rec_ctx->pb, filename, AVIO_FLAG_WRITE) < 0)
        {
            rec_log("start: avio_open failed\n");
            av_frame_free(&rec_yuv);
            sws_freeContext(rec_sws);
            rec_sws = NULL;
            avcodec_free_context(&rec_enc_ctx);
            avformat_free_context(rec_ctx);
            rec_ctx = NULL;
            rec_stream = NULL;
            return false;
        }
    }
    ret = avformat_write_header(rec_ctx, NULL);
    if (ret < 0)
    {
        rec_log("start: write_header failed ret=%d\n", ret);
        if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&rec_ctx->pb);
        av_frame_free(&rec_yuv);
        sws_freeContext(rec_sws);
        rec_sws = NULL;
        avcodec_free_context(&rec_enc_ctx);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }

    rec_pkt = av_packet_alloc();
    if (rec_pkt == NULL)
    {
        rec_log("start: av_packet_alloc failed\n");
        if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&rec_ctx->pb);
        av_frame_free(&rec_yuv);
        sws_freeContext(rec_sws);
        rec_sws = NULL;
        avcodec_free_context(&rec_enc_ctx);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        return false;
    }
    rec_pts = 0;
    rec_write_count = 0;

    pthread_mutex_lock(&rec_frame_mutex);
    rec_frame_pending = false;
    rec_thread_running = true;
    processed_recording = true;
    pthread_mutex_unlock(&rec_frame_mutex);

    if (pthread_create(&rec_thread, NULL, rec_thread_func, NULL) != 0)
    {
        rec_log("start: pthread_create failed\n");
        pthread_mutex_lock(&rec_frame_mutex);
        processed_recording = false;
        rec_thread_running = false;
        pthread_mutex_unlock(&rec_frame_mutex);
        if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&rec_ctx->pb);
        av_frame_free(&rec_yuv);
        sws_freeContext(rec_sws);
        rec_sws = NULL;
        avcodec_free_context(&rec_enc_ctx);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        rec_stream = NULL;
        av_packet_free(&rec_pkt);
        rec_pkt = NULL;
        return false;
    }

    rec_log("start: ok\n");
    return true;
}

void codec_writeProcessedFrame(const uint8_t *bgra)
{
    pthread_mutex_lock(&rec_frame_mutex);
    if (!processed_recording || rec_frame_pending)
    {
        // 编码线程还在处理上一帧，直接丢这一帧，保证视频显示不等待
        pthread_mutex_unlock(&rec_frame_mutex);
        return;
    }

    // 直接拷贝 BGRA，色彩空间/像素格式转换在编码线程里用 sws 完成
    memcpy(rec_bgra, bgra, 320 * 240 * 4);

    rec_frame_pending = true;
    pthread_cond_signal(&rec_frame_cond);
    pthread_mutex_unlock(&rec_frame_mutex);
}

void codec_stopProcessedRecording()
{
    pthread_mutex_lock(&rec_frame_mutex);
    if (!processed_recording)
    {
        pthread_mutex_unlock(&rec_frame_mutex);
        return;
    }
    processed_recording = false;
    rec_thread_running = false;
    pthread_cond_broadcast(&rec_frame_cond);
    pthread_mutex_unlock(&rec_frame_mutex);

    pthread_join(rec_thread, NULL);

    // 冲刷编码器缓冲区里剩余的帧
    rec_encode_and_write(NULL);

    rec_log("stop: frames=%d pts=%lld\n", rec_write_count, (long long)rec_pts);
    int ret = av_write_trailer(rec_ctx);
    rec_log("stop: write_trailer ret=%d\n", ret);
    if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&rec_ctx->pb);

    if (rec_yuv != NULL)
        av_frame_free(&rec_yuv);
    if (rec_sws != NULL)
    {
        sws_freeContext(rec_sws);
        rec_sws = NULL;
    }
    if (rec_enc_ctx != NULL)
        avcodec_free_context(&rec_enc_ctx);

    avformat_free_context(rec_ctx);
    rec_ctx = NULL;
    rec_stream = NULL;

    av_packet_free(&rec_pkt);
    rec_pkt = NULL;

    rec_log("stop: done\n");
}

AVFrame *codec_getFrame()
{
    int ret;
    while (av_read_frame(input_ctx, packet) >= 0)
    {
        if (packet->stream_index == video_stream_index)
        {
            // 在这里转储mjpeg packet
            if (packet_dumping == true)
            {
                if (output_ctx != NULL)
                {
                    av_write_frame(output_ctx, packet);
                }
            }
            // packet输入解码器
            ret = avcodec_send_packet(decoder_ctx, packet);
            if (ret < 0)
            {
                fprintf(stderr, "Error sending packet for decoding.\n");
                break;
            }
            while (ret >= 0)
            {
                // 读取解码器输出
                ret = avcodec_receive_frame(decoder_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0)
                {
                    fprintf(stderr, "Error during decoding.\n");
                    return NULL;
                }

                AVFrame *scaled_frame = av_frame_alloc();
                scaled_frame->format = AV_PIX_FMT_BGRA;
                scaled_frame->width = decoder_ctx->width;
                scaled_frame->height = decoder_ctx->height;
                av_frame_get_buffer(scaled_frame, 0);
                // 色彩空间转换
                sws_scale(sws_ctx, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                          scaled_frame->data, scaled_frame->linesize);

                scaled_frame->pts = frame->pts;
                // scaled_frame：转换后的帧
                // 不考虑出现需解码多个帧的情况，直接返回
                av_packet_unref(packet);
                return scaled_frame;
            }
        }
        av_packet_unref(packet);
    }
    printf("got nothing\n");
    return NULL;
}
