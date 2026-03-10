#include "pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rga/RgaApi.h>
#include <rga/im2d.h>

struct pipeline {
    /* Decoders (RKVDEC2 — separate contexts for H.264 and H.265) */
    MppCtx   dec_h264_ctx;
    MppApi  *dec_h264_mpi;
    MppCtx   dec_h265_ctx;
    MppApi  *dec_h265_mpi;

    /* Encoder (Hantro H1 — H.264 only, RK3566 has no HW H.265 encode) */
    MppCtx   enc_ctx;
    MppApi  *enc_mpi;
    MppEncCfg enc_cfg;

    /* Buffers */
    MppBufferGroup buf_group;
    MppFrame dec_frame;      /* last decoded frame */
    MppFrame comp_frame;     /* composited output frame */
    MppBuffer comp_buf;      /* composite frame buffer */

    /* Output */
    uint8_t *enc_output;
    size_t   enc_output_size;

    int out_w;
    int out_h;
    int fps;
};

static int init_mpp_decoder(MppCtx *ctx, MppApi **mpi, MppCodingType coding,
                             const char *name)
{
    MPP_RET ret;

    ret = mpp_create(ctx, mpi);
    if (ret != MPP_OK) {
        fprintf(stderr, "pipeline: mpp_create %s decoder failed: %d\n", name, ret);
        return -1;
    }

    ret = mpp_init(*ctx, MPP_CTX_DEC, coding);
    if (ret != MPP_OK) {
        fprintf(stderr, "pipeline: mpp_init %s decoder failed: %d\n", name, ret);
        return -1;
    }

    /* Enable split mode for fragmented input */
    int need_split = 1;
    MppParam param = &need_split;
    (*mpi)->control(*ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, param);

    printf("pipeline: %s decoder initialized (RKVDEC2)\n", name);
    return 0;
}

static int init_decoders(pipeline_t *p)
{
    /* H.264 decoder — used for Android Auto input (always H.264) */
    if (init_mpp_decoder(&p->dec_h264_ctx, &p->dec_h264_mpi,
                          MPP_VIDEO_CodingAVC, "H.264") < 0)
        return -1;

    /* H.265 decoder — used for CarPlay/AirPlay input (newer iPhones send HEVC) */
    if (init_mpp_decoder(&p->dec_h265_ctx, &p->dec_h265_mpi,
                          MPP_VIDEO_CodingHEVC, "H.265") < 0)
        return -1;

    return 0;
}

static int init_encoder(pipeline_t *p)
{
    MPP_RET ret;

    ret = mpp_create(&p->enc_ctx, &p->enc_mpi);
    if (ret != MPP_OK) {
        fprintf(stderr, "pipeline: mpp_create encoder failed: %d\n", ret);
        return -1;
    }

    /*
     * Encode is H.264 ONLY — the Hantro H1 on RK3566 does not support H.265.
     * Output to car must be H.264 anyway (Android Auto protocol requirement).
     */
    ret = mpp_init(p->enc_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        fprintf(stderr, "pipeline: mpp_init encoder failed: %d\n", ret);
        return -1;
    }

    /* Configure encoder */
    ret = mpp_enc_cfg_init(&p->enc_cfg);
    if (ret != MPP_OK) {
        fprintf(stderr, "pipeline: mpp_enc_cfg_init failed: %d\n", ret);
        return -1;
    }

    mpp_enc_cfg_set_s32(p->enc_cfg, "prep:width", p->out_w);
    mpp_enc_cfg_set_s32(p->enc_cfg, "prep:height", p->out_h);
    mpp_enc_cfg_set_s32(p->enc_cfg, "prep:hor_stride", p->out_w);
    mpp_enc_cfg_set_s32(p->enc_cfg, "prep:ver_stride", p->out_h);
    mpp_enc_cfg_set_s32(p->enc_cfg, "prep:format", MPP_FMT_YUV420SP); /* NV12 */

    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:bps_target", 4000000);  /* 4 Mbps */
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:bps_max", 6000000);
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:bps_min", 2000000);
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:fps_in_num", p->fps);
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:fps_out_num", p->fps);
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(p->enc_cfg, "rc:gop", p->fps); /* keyframe every second */

    mpp_enc_cfg_set_s32(p->enc_cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(p->enc_cfg, "h264:profile", 100); /* High profile */
    mpp_enc_cfg_set_s32(p->enc_cfg, "h264:level", 31);    /* Level 3.1 */
    mpp_enc_cfg_set_s32(p->enc_cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(p->enc_cfg, "h264:trans8x8", 1);

    ret = p->enc_mpi->control(p->enc_ctx, MPP_ENC_SET_CFG, p->enc_cfg);
    if (ret != MPP_OK) {
        fprintf(stderr, "pipeline: encoder config failed: %d\n", ret);
        return -1;
    }

    /* Allocate output buffer */
    p->enc_output_size = p->out_w * p->out_h; /* generous, H.264 is smaller */
    p->enc_output = malloc(p->enc_output_size);

    printf("pipeline: H.264 encoder initialized (Hantro H1) %dx%d@%dfps\n",
           p->out_w, p->out_h, p->fps);
    printf("pipeline: NOTE — encode is H.264 only (no HW H.265 encode on RK3566)\n");
    return 0;
}

pipeline_t *pipeline_init(int output_w, int output_h, int fps)
{
    pipeline_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->out_w = output_w;
    p->out_h = output_h;
    p->fps = fps;

    /* Buffer group for intermediate frames */
    if (mpp_buffer_group_get_internal(&p->buf_group, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        fprintf(stderr, "pipeline: buffer group init failed\n");
        free(p);
        return NULL;
    }

    /* Allocate composite frame buffer (NV12 = w * h * 1.5) */
    size_t comp_size = output_w * output_h * 3 / 2;
    if (mpp_buffer_get(p->buf_group, &p->comp_buf, comp_size) != MPP_OK) {
        fprintf(stderr, "pipeline: composite buffer alloc failed\n");
        mpp_buffer_group_put(p->buf_group);
        free(p);
        return NULL;
    }

    if (init_decoders(p) < 0 || init_encoder(p) < 0) {
        pipeline_destroy(p);
        return NULL;
    }

    printf("pipeline: initialized %dx%d@%dfps (decode: H.264+H.265, encode: H.264)\n",
           output_w, output_h, fps);
    return p;
}

int pipeline_decode(pipeline_t *p, const uint8_t *data, size_t len,
                     input_codec_t codec)
{
    /* Select decoder based on input codec */
    MppCtx ctx;
    MppApi *mpi;

    if (codec == CODEC_H265) {
        ctx = p->dec_h265_ctx;
        mpi = p->dec_h265_mpi;
    } else {
        ctx = p->dec_h264_ctx;
        mpi = p->dec_h264_mpi;
    }

    MppPacket pkt = NULL;
    MPP_RET ret;

    ret = mpp_packet_init(&pkt, (void *)data, len);
    if (ret != MPP_OK) return -1;

    mpp_packet_set_pts(pkt, 0);
    mpp_packet_set_dts(pkt, 0);

    /* Send packet to decoder */
    ret = mpi->decode_put_packet(ctx, pkt);
    mpp_packet_deinit(&pkt);
    if (ret != MPP_OK) return -1;

    /* Get decoded frame — same YUV output regardless of H.264 or H.265 input */
    MppFrame frame = NULL;
    ret = mpi->decode_get_frame(ctx, &frame);
    if (ret != MPP_OK || !frame) return -1;

    /* Store for compositing */
    if (p->dec_frame)
        mpp_frame_deinit(&p->dec_frame);
    p->dec_frame = frame;

    return 0;
}

int pipeline_composite(pipeline_t *p, layout_mode_t layout,
                        const uint8_t *camera_nv12, int cam_w, int cam_h)
{
    void *comp_ptr = mpp_buffer_get_ptr(p->comp_buf);
    int comp_fd = mpp_buffer_get_fd(p->comp_buf);
    (void)comp_ptr;

    if (layout == LAYOUT_FULL_PRIMARY && p->dec_frame) {
        /* Full screen phone video — just copy/scale decoded frame */
        MppBuffer dec_buf = mpp_frame_get_buffer(p->dec_frame);
        int src_w = mpp_frame_get_width(p->dec_frame);
        int src_h = mpp_frame_get_height(p->dec_frame);
        int src_stride = mpp_frame_get_hor_stride(p->dec_frame);

        rga_buffer_t src = wrapbuffer_fd(mpp_buffer_get_fd(dec_buf),
                                          src_w, src_h, RK_FORMAT_YCbCr_420_SP);
        src.wstride = src_stride;
        src.hstride = mpp_frame_get_ver_stride(p->dec_frame);

        rga_buffer_t dst = wrapbuffer_fd(comp_fd,
                                          p->out_w, p->out_h, RK_FORMAT_YCbCr_420_SP);
        dst.wstride = p->out_w;
        dst.hstride = p->out_h;

        imresize(src, dst);

    } else if (layout == LAYOUT_FULL_CAMERA && camera_nv12) {
        /* Full screen camera — scale camera NV12 to output */
        rga_buffer_t src = wrapbuffer_virtualaddr((void *)camera_nv12,
                                                    cam_w, cam_h, RK_FORMAT_YCbCr_420_SP);
        rga_buffer_t dst = wrapbuffer_fd(comp_fd,
                                          p->out_w, p->out_h, RK_FORMAT_YCbCr_420_SP);
        dst.wstride = p->out_w;
        dst.hstride = p->out_h;

        imresize(src, dst);

    } else if (layout == LAYOUT_SPLIT_LEFT_RIGHT) {
        /* Split screen: phone left (640x720), camera right (640x720) */
        int half_w = p->out_w / 2;

        /* Left half: phone video */
        if (p->dec_frame) {
            MppBuffer dec_buf = mpp_frame_get_buffer(p->dec_frame);
            int src_w = mpp_frame_get_width(p->dec_frame);
            int src_h = mpp_frame_get_height(p->dec_frame);

            rga_buffer_t src = wrapbuffer_fd(mpp_buffer_get_fd(dec_buf),
                                              src_w, src_h, RK_FORMAT_YCbCr_420_SP);
            src.wstride = mpp_frame_get_hor_stride(p->dec_frame);
            src.hstride = mpp_frame_get_ver_stride(p->dec_frame);

            rga_buffer_t dst = wrapbuffer_fd(comp_fd,
                                              p->out_w, p->out_h, RK_FORMAT_YCbCr_420_SP);
            dst.wstride = p->out_w;
            dst.hstride = p->out_h;

            im_rect dst_rect = {0, 0, half_w, p->out_h};
            improcess(src, dst, (rga_buffer_t){0},
                      (im_rect){0, 0, src_w, src_h}, dst_rect, (im_rect){0},
                      0);
        }

        /* Right half: camera */
        if (camera_nv12) {
            rga_buffer_t src = wrapbuffer_virtualaddr((void *)camera_nv12,
                                                        cam_w, cam_h, RK_FORMAT_YCbCr_420_SP);

            rga_buffer_t dst = wrapbuffer_fd(comp_fd,
                                              p->out_w, p->out_h, RK_FORMAT_YCbCr_420_SP);
            dst.wstride = p->out_w;
            dst.hstride = p->out_h;

            im_rect dst_rect = {half_w, 0, half_w, p->out_h};
            improcess(src, dst, (rga_buffer_t){0},
                      (im_rect){0, 0, cam_w, cam_h}, dst_rect, (im_rect){0},
                      0);
        }
    }

    return 0;
}

const uint8_t *pipeline_encode(pipeline_t *p, size_t *out_len)
{
    /* Wrap composite buffer as MppFrame for encoder */
    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, p->out_w);
    mpp_frame_set_height(frame, p->out_h);
    mpp_frame_set_hor_stride(frame, p->out_w);
    mpp_frame_set_ver_stride(frame, p->out_h);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, p->comp_buf);

    /* Send frame to encoder — always H.264 output (Hantro H1) */
    MPP_RET ret = p->enc_mpi->encode_put_frame(p->enc_ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret != MPP_OK) {
        *out_len = 0;
        return NULL;
    }

    /* Get encoded packet */
    MppPacket pkt = NULL;
    ret = p->enc_mpi->encode_get_packet(p->enc_ctx, &pkt);
    if (ret != MPP_OK || !pkt) {
        *out_len = 0;
        return NULL;
    }

    size_t len = mpp_packet_get_length(pkt);
    void *data = mpp_packet_get_data(pkt);

    if (len > p->enc_output_size) {
        p->enc_output_size = len;
        p->enc_output = realloc(p->enc_output, len);
    }
    memcpy(p->enc_output, data, len);
    *out_len = len;

    mpp_packet_deinit(&pkt);
    return p->enc_output;
}

const uint8_t *pipeline_encode_camera_only(pipeline_t *p,
    const uint8_t *nv12, int w, int h, size_t *out_len)
{
    /* Scale camera to composite buffer via RGA */
    rga_buffer_t src = wrapbuffer_virtualaddr((void *)nv12,
                                                w, h, RK_FORMAT_YCbCr_420_SP);
    int comp_fd = mpp_buffer_get_fd(p->comp_buf);
    rga_buffer_t dst = wrapbuffer_fd(comp_fd,
                                      p->out_w, p->out_h, RK_FORMAT_YCbCr_420_SP);
    dst.wstride = p->out_w;
    dst.hstride = p->out_h;
    imresize(src, dst);

    return pipeline_encode(p, out_len);
}

void pipeline_destroy(pipeline_t *p)
{
    if (!p) return;

    if (p->dec_frame)
        mpp_frame_deinit(&p->dec_frame);

    if (p->dec_h264_ctx) {
        p->dec_h264_mpi->reset(p->dec_h264_ctx);
        mpp_destroy(p->dec_h264_ctx);
    }
    if (p->dec_h265_ctx) {
        p->dec_h265_mpi->reset(p->dec_h265_ctx);
        mpp_destroy(p->dec_h265_ctx);
    }
    if (p->enc_ctx) {
        p->enc_mpi->reset(p->enc_ctx);
        mpp_destroy(p->enc_ctx);
    }
    if (p->enc_cfg)
        mpp_enc_cfg_deinit(p->enc_cfg);

    if (p->comp_buf)
        mpp_buffer_put(p->comp_buf);
    if (p->buf_group)
        mpp_buffer_group_put(p->buf_group);

    free(p->enc_output);
    free(p);
    printf("pipeline: destroyed\n");
}
