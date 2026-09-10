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
#include "utils/tiny_jpeg.h"
#include <vector>
#include <stdarg.h>
#include <mutex>
static AVFormatContext *rec_ctx = NULL;
static AVStream *rec_stream = NULL;
static AVPacket *rec_pkt = NULL;
static std::vector<uint8_t> rec_jpeg_buf;
static uint8_t rec_rgba[320 * 240 * 4];
static bool processed_recording = false;
static int64_t rec_pts = 0;
static int rec_write_count = 0;
static std::mutex rec_mutex; // 保护录像状态，防止视频线程写帧与 UI 线程启停冲突

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

static void rec_write_cb(void *context, void *data, int size)
{
    if (context != NULL && data != NULL && size > 0)
    {
        std::vector<uint8_t> *buf = (std::vector<uint8_t> *)context;
        uint8_t *p = (uint8_t *)data;
        buf->insert(buf->end(), p, p + size);
    }
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

// 调用前必须已持有 rec_mutex
static void codec_stopProcessedRecording_locked()
{
    if (!processed_recording)
        return;

    rec_log("stop: frames=%d pts=%lld\n", rec_write_count, (long long)rec_pts);
    int ret = av_write_trailer(rec_ctx);
    rec_log("stop: write_trailer ret=%d\n", ret);
    if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&rec_ctx->pb);
    avformat_free_context(rec_ctx);
    rec_ctx = NULL;
    rec_stream = NULL;

    av_packet_free(&rec_pkt);
    rec_pkt = NULL;

    processed_recording = false;
    rec_log("stop: done\n");
}

bool codec_startProcessedRecording(const char *filename, int width, int height)
{
    (void)width;
    (void)height;
    std::lock_guard<std::mutex> lock(rec_mutex);
    rec_log("start: %s\n", filename);
    if (processed_recording)
        codec_stopProcessedRecording_locked();

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

    // 我们写入的是 320x240 的 tiny_jpeg 编码帧，参数必须与实际帧一致
    rec_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    rec_stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    rec_stream->codecpar->width = 320;
    rec_stream->codecpar->height = 240;
    rec_stream->time_base = (AVRational){1, 25};

    if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&rec_ctx->pb, filename, AVIO_FLAG_WRITE) < 0)
        {
            rec_log("start: avio_open failed\n");
            avformat_free_context(rec_ctx);
            rec_ctx = NULL;
            return false;
        }
    }
    ret = avformat_write_header(rec_ctx, NULL);
    if (ret < 0)
    {
        rec_log("start: write_header failed ret=%d\n", ret);
        if (!(rec_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&rec_ctx->pb);
        avformat_free_context(rec_ctx);
        rec_ctx = NULL;
        return false;
    }

    rec_pkt = av_packet_alloc();
    rec_pts = 0;
    rec_write_count = 0;
    processed_recording = true;
    rec_log("start: ok\n");
    return true;
}

void codec_writeProcessedFrame(const uint8_t *bgra)
{
    std::lock_guard<std::mutex> lock(rec_mutex);
    if (!processed_recording)
        return;

    // BGRA -> RGBA（tiny_jpeg 只支持 RGB/RGBA 顺序）
    for (int i = 0; i < 320 * 240; i++)
    {
        rec_rgba[i * 4 + 0] = bgra[i * 4 + 2];
        rec_rgba[i * 4 + 1] = bgra[i * 4 + 1];
        rec_rgba[i * 4 + 2] = bgra[i * 4 + 0];
        rec_rgba[i * 4 + 3] = bgra[i * 4 + 3];
    }

    rec_jpeg_buf.clear();
    if (tje_encode_with_func(rec_write_cb, &rec_jpeg_buf, 3, 320, 240, 4, rec_rgba) == 0)
    {
        rec_log("write: tiny_jpeg encode failed\n");
        fprintf(stderr, "processed recording: tiny_jpeg encode failed\n");
        return;
    }
    if (rec_jpeg_buf.empty())
    {
        rec_log("write: empty jpeg\n");
        return;
    }

    av_packet_unref(rec_pkt);
    if (av_new_packet(rec_pkt, (int)rec_jpeg_buf.size()) < 0)
    {
        rec_log("write: av_new_packet failed size=%d\n", (int)rec_jpeg_buf.size());
        return;
    }
    memcpy(rec_pkt->data, rec_jpeg_buf.data(), rec_jpeg_buf.size());
    rec_pkt->stream_index = rec_stream->index;
    rec_pkt->pts = rec_pts++;
    rec_pkt->dts = rec_pkt->pts;
    av_packet_rescale_ts(rec_pkt, rec_stream->time_base, rec_stream->time_base);
    int ret = av_interleaved_write_frame(rec_ctx, rec_pkt);
    if (rec_write_count < 5 || ret < 0)
        rec_log("write: count=%d size=%d ret=%d\n", rec_write_count, (int)rec_jpeg_buf.size(), ret);
    rec_write_count++;
}

void codec_stopProcessedRecording()
{
    std::lock_guard<std::mutex> lock(rec_mutex);
    codec_stopProcessedRecording_locked();
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
