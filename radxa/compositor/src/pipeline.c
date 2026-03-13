/*
 * pipeline.c — FFmpeg-based video pipeline
 *
 * Portable baseline implementation using libavcodec for H.264/H.265
 * decode and H.264 encode, libswscale for YUV scaling/compositing.
 *
 * On Allwinner A733, FFmpeg can use CedarVE hardware acceleration via
 * the V4L2 M2M (cedrus) stateless decoder when available.  The encoder
 * defaults to libx264 software; hardware encode can be added later.
 *
 * Flow:
 *   Input H.264/H.265 → avcodec decode → YUV420P frame
 *   Camera NV12        → (passed in raw)
 *   Both frames        → sws_scale composite → YUV420P frame
 *   Composited frame   → avcodec encode → H.264 output
 */
#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

struct pipeline {
    /* Decoders */
    AVCodecContext *dec_h264;
    AVCodecContext *dec_h265;

    /* Encoder (H.264, typically libx264) */
    AVCodecContext *enc;

    /* Last decoded frame (format depends on decoder, usually YUV420P) */
    AVFrame *dec_frame;
    int dec_valid;

    /* Composite output (YUV420P, out_w × out_h) */
    AVFrame *comp_frame;

    /* Temp frame for sub-region scaling in split mode */
    AVFrame *tmp_frame;

    /* Packet for encoder output */
    AVPacket *enc_pkt;

    /* Encoded output buffer (caller-safe copy) */
    uint8_t *enc_output;
    size_t enc_output_cap;

    int out_w, out_h, fps;
    int64_t pts;
};

/* ---- Scale source frame into a destination AVFrame ---- */

static int scale_into(const uint8_t *const src_data[], const int src_linesize[],
                       int src_w, int src_h, enum AVPixelFormat src_fmt,
                       AVFrame *dst)
{
    struct SwsContext *sws = sws_getContext(
        src_w, src_h, src_fmt,
        dst->width, dst->height, (enum AVPixelFormat)dst->format,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return -1;

    sws_scale(sws, src_data, src_linesize, 0, src_h,
              dst->data, dst->linesize);
    sws_freeContext(sws);
    return 0;
}

/* ---- Copy YUV420P sub-frame into a region of the composite ---- */

static void blit_yuv420p(AVFrame *dst, const AVFrame *src, int x_offset)
{
    int w = src->width;
    int h = src->height;

    /* Y plane */
    for (int y = 0; y < h; y++) {
        memcpy(dst->data[0] + y * dst->linesize[0] + x_offset,
               src->data[0] + y * src->linesize[0], w);
    }

    /* U plane (half resolution) */
    int hw = w / 2, hh = h / 2, hx = x_offset / 2;
    for (int y = 0; y < hh; y++) {
        memcpy(dst->data[1] + y * dst->linesize[1] + hx,
               src->data[1] + y * src->linesize[1], hw);
    }

    /* V plane (half resolution) */
    for (int y = 0; y < hh; y++) {
        memcpy(dst->data[2] + y * dst->linesize[2] + hx,
               src->data[2] + y * src->linesize[2], hw);
    }
}

/* ---- Decoder init ---- */

static AVCodecContext *open_decoder(enum AVCodecID id, const char *name)
{
    AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        fprintf(stderr, "pipeline: %s decoder not found\n", name);
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "pipeline: %s decoder open failed\n", name);
        avcodec_free_context(&ctx);
        return NULL;
    }

    printf("pipeline: %s decoder initialized (%s)\n", name, codec->name);
    return ctx;
}

/* ---- Encoder init ---- */

static AVCodecContext *open_encoder(int w, int h, int fps)
{
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "pipeline: H.264 encoder not found\n");
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;

    ctx->width       = w;
    ctx->height      = h;
    ctx->time_base   = (AVRational){1, fps};
    ctx->framerate   = (AVRational){fps, 1};
    ctx->pix_fmt     = AV_PIX_FMT_YUV420P;
    ctx->bit_rate    = 4000000;    /* 4 Mbps */
    ctx->gop_size    = fps;        /* keyframe every second */
    ctx->max_b_frames = 0;         /* low latency */

    /* libx264-specific low-latency settings (ignored by other encoders) */
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "pipeline: H.264 encoder open failed\n");
        avcodec_free_context(&ctx);
        return NULL;
    }

    printf("pipeline: H.264 encoder initialized (%s) %dx%d@%dfps\n",
           codec->name, w, h, fps);
    return ctx;
}

/* ---- Allocate a YUV420P AVFrame ---- */

static AVFrame *alloc_yuv420p_frame(int w, int h)
{
    AVFrame *f = av_frame_alloc();
    if (!f) return NULL;

    f->format = AV_PIX_FMT_YUV420P;
    f->width  = w;
    f->height = h;

    if (av_frame_get_buffer(f, 32) < 0) {
        av_frame_free(&f);
        return NULL;
    }
    return f;
}

/* ---- Public API ---- */

pipeline_t *pipeline_init(int output_w, int output_h, int fps)
{
    pipeline_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->out_w = output_w;
    p->out_h = output_h;
    p->fps   = fps;

    /* H.264 decoder is required (AA always sends H.264) */
    p->dec_h264 = open_decoder(AV_CODEC_ID_H264, "H.264");
    if (!p->dec_h264) { free(p); return NULL; }

    /* H.265 decoder is optional (CarPlay may send HEVC) */
    p->dec_h265 = open_decoder(AV_CODEC_ID_HEVC, "H.265");

    p->enc = open_encoder(output_w, output_h, fps);
    if (!p->enc) { pipeline_destroy(p); return NULL; }

    p->dec_frame  = av_frame_alloc();
    p->comp_frame = alloc_yuv420p_frame(output_w, output_h);
    p->tmp_frame  = alloc_yuv420p_frame(output_w / 2, output_h);
    p->enc_pkt    = av_packet_alloc();

    if (!p->dec_frame || !p->comp_frame || !p->tmp_frame || !p->enc_pkt) {
        pipeline_destroy(p);
        return NULL;
    }

    p->enc_output_cap = output_w * output_h;
    p->enc_output = malloc(p->enc_output_cap);

    printf("pipeline: initialized %dx%d@%dfps (FFmpeg software)\n",
           output_w, output_h, fps);
    return p;
}

int pipeline_decode(pipeline_t *p, const uint8_t *data, size_t len,
                     input_codec_t codec)
{
    AVCodecContext *dec = (codec == CODEC_H265 && p->dec_h265)
                          ? p->dec_h265 : p->dec_h264;

    /* Wrap input data in a packet without copying */
    AVPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.data = (uint8_t *)data;
    pkt.size = (int)len;

    int ret = avcodec_send_packet(dec, &pkt);
    if (ret < 0) return -1;

    av_frame_unref(p->dec_frame);
    ret = avcodec_receive_frame(dec, p->dec_frame);
    if (ret < 0) {
        p->dec_valid = 0;
        return -1;
    }

    p->dec_valid = 1;
    return 0;
}

int pipeline_composite(pipeline_t *p, layout_mode_t layout,
                        const uint8_t *camera_nv12, int cam_w, int cam_h)
{
    /* Clear composite to black (Y=0, U=V=128) */
    memset(p->comp_frame->data[0], 0,
           p->comp_frame->linesize[0] * p->out_h);
    memset(p->comp_frame->data[1], 128,
           p->comp_frame->linesize[1] * (p->out_h / 2));
    memset(p->comp_frame->data[2], 128,
           p->comp_frame->linesize[2] * (p->out_h / 2));

    if (layout == LAYOUT_FULL_PRIMARY && p->dec_valid) {
        /* Full screen phone video */
        scale_into((const uint8_t *const *)p->dec_frame->data,
                   p->dec_frame->linesize,
                   p->dec_frame->width, p->dec_frame->height,
                   (enum AVPixelFormat)p->dec_frame->format,
                   p->comp_frame);

    } else if (layout == LAYOUT_FULL_CAMERA && camera_nv12) {
        /* Full screen camera */
        const uint8_t *planes[2] = { camera_nv12, camera_nv12 + cam_w * cam_h };
        int strides[2] = { cam_w, cam_w };

        scale_into(planes, strides, cam_w, cam_h, AV_PIX_FMT_NV12,
                   p->comp_frame);

    } else if (layout == LAYOUT_SPLIT_LEFT_RIGHT) {
        /* Left half: phone video */
        if (p->dec_valid) {
            scale_into((const uint8_t *const *)p->dec_frame->data,
                       p->dec_frame->linesize,
                       p->dec_frame->width, p->dec_frame->height,
                       (enum AVPixelFormat)p->dec_frame->format,
                       p->tmp_frame);
            blit_yuv420p(p->comp_frame, p->tmp_frame, 0);
        }

        /* Right half: camera */
        if (camera_nv12) {
            const uint8_t *planes[2] = {
                camera_nv12, camera_nv12 + cam_w * cam_h
            };
            int strides[2] = { cam_w, cam_w };

            scale_into(planes, strides, cam_w, cam_h, AV_PIX_FMT_NV12,
                       p->tmp_frame);
            blit_yuv420p(p->comp_frame, p->tmp_frame, p->out_w / 2);
        }
    }

    return 0;
}

const uint8_t *pipeline_encode(pipeline_t *p, size_t *out_len)
{
    *out_len = 0;

    p->comp_frame->pts = p->pts++;

    int ret = avcodec_send_frame(p->enc, p->comp_frame);
    if (ret < 0) return NULL;

    av_packet_unref(p->enc_pkt);
    ret = avcodec_receive_packet(p->enc, p->enc_pkt);
    if (ret < 0) return NULL;

    /* Copy to stable output buffer */
    if ((size_t)p->enc_pkt->size > p->enc_output_cap) {
        p->enc_output_cap = p->enc_pkt->size;
        p->enc_output = realloc(p->enc_output, p->enc_output_cap);
    }
    memcpy(p->enc_output, p->enc_pkt->data, p->enc_pkt->size);
    *out_len = p->enc_pkt->size;

    return p->enc_output;
}

const uint8_t *pipeline_encode_camera_only(pipeline_t *p,
    const uint8_t *nv12, int w, int h, size_t *out_len)
{
    const uint8_t *planes[2] = { nv12, nv12 + w * h };
    int strides[2] = { w, w };

    scale_into(planes, strides, w, h, AV_PIX_FMT_NV12, p->comp_frame);
    return pipeline_encode(p, out_len);
}

void pipeline_destroy(pipeline_t *p)
{
    if (!p) return;

    if (p->dec_h264)    avcodec_free_context(&p->dec_h264);
    if (p->dec_h265)    avcodec_free_context(&p->dec_h265);
    if (p->enc)         avcodec_free_context(&p->enc);
    if (p->dec_frame)   av_frame_free(&p->dec_frame);
    if (p->comp_frame)  av_frame_free(&p->comp_frame);
    if (p->tmp_frame)   av_frame_free(&p->tmp_frame);
    if (p->enc_pkt)     av_packet_free(&p->enc_pkt);
    free(p->enc_output);
    free(p);
    printf("pipeline: destroyed\n");
}
