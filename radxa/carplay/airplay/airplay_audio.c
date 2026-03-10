/*
 * airplay_audio.c — AirPlay Audio Stream Handler
 *
 * Receives RTP audio packets from the iPhone, decodes them with libavcodec,
 * and plays them via ALSA.
 *
 * Architecture:
 *   recv_thread   — receives UDP RTP packets, decodes with libavcodec,
 *                   writes PCM into the ring buffer
 *   play_thread   — reads PCM from ring buffer, applies volume scaling,
 *                   writes to ALSA via snd_pcm_writei()
 *
 * The ring buffer provides ~200ms of audio headroom to absorb network jitter
 * and decoder latency.
 *
 * Dependencies: libavcodec, libavutil, libasound (ALSA)
 */

#include "airplay_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* libavcodec/libavutil — linked via Makefile */
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

/* ALSA */
#include <alsa/asoundlib.h>

/* Global volume (accessed from RTSP SET_PARAMETER via airplay_audio_set_volume()) */
static volatile float g_volume_linear = 1.0f;

/* -----------------------------------------------------------------------
 * RTP header parsing
 * ----------------------------------------------------------------------- */

#define RTP_HDR_LEN 12

typedef struct {
    uint8_t  version;  /* should be 2 */
    uint8_t  payload_type;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_header_t;

static int rtp_parse_header(const uint8_t *buf, size_t len, rtp_header_t *hdr)
{
    if (len < RTP_HDR_LEN) return -1;
    hdr->version      = (buf[0] >> 6) & 0x3;
    hdr->payload_type = buf[1] & 0x7F;
    hdr->seq          = ((uint16_t)buf[2] << 8) | buf[3];
    hdr->timestamp    = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                        ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
    hdr->ssrc         = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                        ((uint32_t)buf[10]<<  8) |  (uint32_t)buf[11];
    if (hdr->version != 2) return -1;
    return 0;
}

/* -----------------------------------------------------------------------
 * Ring buffer
 * ----------------------------------------------------------------------- */

static size_t ring_available_write(const audio_ring_t *r)
{
    size_t wp = r->write_pos;
    size_t rp = r->read_pos;
    if (wp >= rp) return AUDIO_RING_BYTES - (wp - rp) - 1;
    return rp - wp - 1;
}

static size_t ring_available_read(const audio_ring_t *r)
{
    size_t wp = r->write_pos;
    size_t rp = r->read_pos;
    if (wp >= rp) return wp - rp;
    return AUDIO_RING_BYTES - (rp - wp);
}

static void ring_write(audio_ring_t *r, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < len; i++) {
        r->buf[r->write_pos] = data[i];
        r->write_pos = (r->write_pos + 1) % AUDIO_RING_BYTES;
    }
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

static size_t ring_read(audio_ring_t *r, uint8_t *out, size_t max_len)
{
    pthread_mutex_lock(&r->lock);
    while (ring_available_read(r) == 0) {
        /* Use timedwait so caller can recheck its running flag on timeout */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000; /* 100ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec  += 1;
            ts.tv_nsec -= 1000000000;
        }
        int rc = pthread_cond_timedwait(&r->cond, &r->lock, &ts);
        if (rc != 0 && ring_available_read(r) == 0) {
            /* Timeout or spurious wakeup with no data — let caller recheck */
            pthread_mutex_unlock(&r->lock);
            return 0;
        }
    }
    size_t avail = ring_available_read(r);
    size_t n = avail < max_len ? avail : max_len;
    for (size_t i = 0; i < n; i++) {
        out[i] = r->buf[r->read_pos];
        r->read_pos = (r->read_pos + 1) % AUDIO_RING_BYTES;
    }
    pthread_mutex_unlock(&r->lock);
    return n;
}

/* -----------------------------------------------------------------------
 * Volume application (in-place, 16-bit signed PCM)
 * ----------------------------------------------------------------------- */

static void apply_volume(uint8_t *pcm, size_t bytes, float vol)
{
    if (vol >= 0.999f) return;  /* Unity gain — skip */
    int16_t *samples = (int16_t *)pcm;
    size_t n = bytes / 2;
    for (size_t i = 0; i < n; i++) {
        int32_t s = (int32_t)samples[i];
        s = (int32_t)(s * vol);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }
}

/* -----------------------------------------------------------------------
 * ALSA setup
 * ----------------------------------------------------------------------- */

static snd_pcm_t *alsa_open(void)
{
    snd_pcm_t *pcm = NULL;
    int ret;

    ret = snd_pcm_open(&pcm, ALSA_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        fprintf(stderr, "audio: snd_pcm_open failed: %s\n", snd_strerror(ret));
        return NULL;
    }

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm, hw_params);
    snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw_params, AUDIO_CHANNELS);

    unsigned int rate = AUDIO_SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, 0);

    /* Buffer size: ~200ms */
    snd_pcm_uframes_t buffer_frames = (AUDIO_SAMPLE_RATE * 200) / 1000;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_frames);

    /* Period: ~20ms */
    snd_pcm_uframes_t period_frames = (AUDIO_SAMPLE_RATE * 20) / 1000;
    snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period_frames, 0);

    ret = snd_pcm_hw_params(pcm, hw_params);
    if (ret < 0) {
        fprintf(stderr, "audio: snd_pcm_hw_params failed: %s\n", snd_strerror(ret));
        snd_pcm_close(pcm);
        return NULL;
    }

    printf("audio: ALSA opened at %u Hz, %d ch, 16-bit LE\n",
           rate, AUDIO_CHANNELS);
    return pcm;
}

/* -----------------------------------------------------------------------
 * libavcodec decoder setup
 * ----------------------------------------------------------------------- */

static int decoder_init(airplay_audio_ctx_t *ctx)
{
    const AVCodec *codec = NULL;

    switch (ctx->format) {
    case AUDIO_FMT_AAC_LC:
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!codec) { fprintf(stderr, "audio: AAC decoder not found\n"); return -1; }
        break;
    case AUDIO_FMT_ALAC:
        codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
        if (!codec) { fprintf(stderr, "audio: ALAC decoder not found\n"); return -1; }
        break;
    case AUDIO_FMT_PCM:
        /* No decoder needed for raw PCM */
        return 0;
    default:
        fprintf(stderr, "audio: unknown format %d\n", ctx->format);
        return -1;
    }

    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx) return -1;

    /* Common CarPlay audio parameters */
    avctx->sample_rate = AUDIO_SAMPLE_RATE;
    av_channel_layout_default(&avctx->ch_layout, AUDIO_CHANNELS);

    if (avcodec_open2(avctx, codec, NULL) < 0) {
        fprintf(stderr, "audio: avcodec_open2 failed\n");
        avcodec_free_context(&avctx);
        return -1;
    }

    ctx->decoder_ctx   = avctx;
    ctx->decoder_frame = av_frame_alloc();
    if (!ctx->decoder_frame) { avcodec_free_context(&avctx); return -1; }

    printf("audio: decoder initialised (%s)\n", codec->name);
    return 0;
}

/*
 * Decode one RTP payload into PCM, then write to ring buffer.
 * Returns 0 on success.
 */
static int decode_and_buffer(airplay_audio_ctx_t *ctx,
                               const uint8_t *payload, size_t payload_len)
{
    if (ctx->format == AUDIO_FMT_PCM) {
        /* Raw PCM — write directly */
        if (ring_available_write(&ctx->ring) >= payload_len) {
            ring_write(&ctx->ring, payload, payload_len);
        } else {
            fprintf(stderr, "audio: ring buffer full, dropping %zu bytes\n",
                    payload_len);
        }
        return 0;
    }

    /* AAC / ALAC: use libavcodec */
    AVCodecContext *avctx = (AVCodecContext *)ctx->decoder_ctx;
    AVFrame        *frame = (AVFrame        *)ctx->decoder_frame;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;

    pkt->data = (uint8_t *)payload;
    pkt->size = (int)payload_len;

    int ret = avcodec_send_packet(avctx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) return -1;

    while (avcodec_receive_frame(avctx, frame) == 0) {
        /* Convert to interleaved S16LE */
        int samples = frame->nb_samples;
        int ch      = frame->ch_layout.nb_channels;
        size_t pcm_bytes = samples * ch * sizeof(int16_t);

        uint8_t *pcm_buf = malloc(pcm_bytes);
        if (!pcm_buf) continue;

        if (frame->format == AV_SAMPLE_FMT_S16) {
            memcpy(pcm_buf, frame->data[0], pcm_bytes);
        } else if (frame->format == AV_SAMPLE_FMT_FLTP) {
            /* Float planar → interleaved S16LE */
            int16_t *out16 = (int16_t *)pcm_buf;
            for (int s = 0; s < samples; s++) {
                for (int c = 0; c < ch; c++) {
                    float *fplane = (float *)frame->data[c];
                    float f = fplane[s];
                    if (f >  1.0f) f =  1.0f;
                    if (f < -1.0f) f = -1.0f;
                    out16[s * ch + c] = (int16_t)(f * 32767.0f);
                }
            }
        } else if (frame->format == AV_SAMPLE_FMT_S16P) {
            /* S16 planar → interleaved */
            int16_t *out16 = (int16_t *)pcm_buf;
            for (int s = 0; s < samples; s++) {
                for (int c = 0; c < ch; c++) {
                    int16_t *plane = (int16_t *)frame->data[c];
                    out16[s * ch + c] = plane[s];
                }
            }
        } else {
            /* Unsupported format — silence */
            memset(pcm_buf, 0, pcm_bytes);
        }

        if (ring_available_write(&ctx->ring) >= pcm_bytes) {
            ring_write(&ctx->ring, pcm_buf, pcm_bytes);
        } else {
            fprintf(stderr, "audio: ring buffer full, dropping frame\n");
        }
        free(pcm_buf);
        av_frame_unref(frame);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Receive thread
 * ----------------------------------------------------------------------- */

static void *recv_thread_fn(void *arg)
{
    airplay_audio_ctx_t *ctx = (airplay_audio_ctx_t *)arg;
    uint8_t pkt[65536];

    printf("audio: receive thread started on port %u\n", ctx->data_port);

    while (ctx->running) {
        ssize_t n = recv(ctx->data_sock, pkt, sizeof(pkt), 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }

        rtp_header_t hdr;
        if (rtp_parse_header(pkt, n, &hdr) < 0) continue;

        /* Log sequence gaps */
        if (!ctx->first_packet) {
            uint16_t expected = (uint16_t)(ctx->last_seq + 1);
            if (hdr.seq != expected) {
                fprintf(stderr, "audio: RTP gap: expected seq %u got %u\n",
                        expected, hdr.seq);
            }
        }
        ctx->last_seq    = hdr.seq;
        ctx->first_packet = false;

        const uint8_t *payload = pkt + RTP_HDR_LEN;
        size_t payload_len     = n - RTP_HDR_LEN;

        decode_and_buffer(ctx, payload, payload_len);
    }

    printf("audio: receive thread exited\n");
    return NULL;
}

/* -----------------------------------------------------------------------
 * Playback thread
 * ----------------------------------------------------------------------- */

static void *play_thread_fn(void *arg)
{
    airplay_audio_ctx_t *ctx = (airplay_audio_ctx_t *)arg;
    snd_pcm_t *pcm = (snd_pcm_t *)ctx->alsa_pcm;

    if (!pcm) {
        fprintf(stderr, "audio: no ALSA handle, playback thread exiting\n");
        return NULL;
    }

    /* Period size in bytes: 20ms of stereo 16-bit */
    snd_pcm_uframes_t period_frames = (AUDIO_SAMPLE_RATE * 20) / 1000;
    size_t period_bytes = period_frames * AUDIO_CHANNELS * sizeof(int16_t);

    uint8_t *buf = malloc(period_bytes);
    if (!buf) return NULL;

    printf("audio: playback thread started\n");

    while (ctx->running) {
        size_t got = ring_read(&ctx->ring, buf, period_bytes);
        if (got == 0) continue;

        /* Apply volume */
        apply_volume(buf, got, g_volume_linear);

        snd_pcm_sframes_t frames = got / (AUDIO_CHANNELS * sizeof(int16_t));
        snd_pcm_sframes_t ret = snd_pcm_writei(pcm, buf, (snd_pcm_uframes_t)frames);
        if (ret == -EPIPE) {
            /* Buffer underrun — try to recover */
            snd_pcm_prepare(pcm);
        } else if (ret < 0) {
            fprintf(stderr, "audio: snd_pcm_writei error: %s\n",
                    snd_strerror((int)ret));
            snd_pcm_recover(pcm, (int)ret, 0);
        }
    }

    free(buf);
    printf("audio: playback thread exited\n");
    return NULL;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int airplay_audio_ctx_init(airplay_audio_ctx_t *ctx,
                            uint16_t data_port,
                            uint16_t control_port,
                            audio_format_t format)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    ctx->data_port    = data_port;
    ctx->control_port = control_port;
    ctx->format       = format;
    ctx->volume       = 1.0f;
    ctx->first_packet = true;
    ctx->running      = 0;
    ctx->data_sock    = -1;
    ctx->control_sock = -1;

    /* Init ring buffer mutex */
    pthread_mutex_init(&ctx->ring.lock, NULL);
    pthread_cond_init(&ctx->ring.cond, NULL);

    /* Open UDP sockets */
    ctx->data_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->data_sock < 0) {
        perror("audio: socket(data)");
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(data_port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(ctx->data_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("audio: bind(data)");
        close(ctx->data_sock);
        ctx->data_sock = -1;
        return -1;
    }

    ctx->control_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->control_sock >= 0) {
        addr.sin_port = htons(control_port);
        bind(ctx->control_sock, (struct sockaddr *)&addr, sizeof(addr));
    }

    /* Initialise decoder */
    if (decoder_init(ctx) < 0) {
        close(ctx->data_sock);
        ctx->data_sock = -1;
        return -1;
    }

    /* Open ALSA */
    ctx->alsa_pcm = alsa_open();
    if (!ctx->alsa_pcm) {
        fprintf(stderr, "audio: ALSA open failed — audio disabled\n");
        /* Non-fatal: continue without audio output */
    }

    printf("audio: context init complete (data_port=%u, format=%d)\n",
           data_port, format);
    return 0;
}

int airplay_audio_start(airplay_audio_ctx_t *ctx)
{
    if (!ctx) return -1;
    ctx->running = 1;

    if (pthread_create(&ctx->recv_thread, NULL, recv_thread_fn, ctx) != 0) {
        perror("audio: pthread_create recv_thread");
        ctx->running = 0;
        return -1;
    }

    if (ctx->alsa_pcm &&
        pthread_create(&ctx->play_thread, NULL, play_thread_fn, ctx) != 0) {
        perror("audio: pthread_create play_thread");
        ctx->running = 0;
        pthread_join(ctx->recv_thread, NULL);
        return -1;
    }

    return 0;
}

void airplay_audio_stop(airplay_audio_ctx_t *ctx)
{
    if (!ctx || !ctx->running) return;
    ctx->running = 0;

    /* Unblock recv thread */
    if (ctx->data_sock >= 0) shutdown(ctx->data_sock, SHUT_RDWR);

    /* Unblock play thread */
    pthread_mutex_lock(&ctx->ring.lock);
    pthread_cond_broadcast(&ctx->ring.cond);
    pthread_mutex_unlock(&ctx->ring.lock);

    pthread_join(ctx->recv_thread, NULL);
    if (ctx->alsa_pcm) pthread_join(ctx->play_thread, NULL);
}

void airplay_audio_set_volume(float vol_db)
{
    /* Convert dB to linear: vol_linear = 10^(vol_db / 20) */
    if (vol_db <= -144.0f) {
        g_volume_linear = 0.0f;   /* mute */
    } else if (vol_db >= 0.0f) {
        g_volume_linear = 1.0f;   /* full volume */
    } else {
        g_volume_linear = powf(10.0f, vol_db / 20.0f);
    }
    printf("audio: volume %.1f dB → %.3f linear\n", vol_db, g_volume_linear);
}

void airplay_audio_ctx_destroy(airplay_audio_ctx_t *ctx)
{
    if (!ctx) return;

    airplay_audio_stop(ctx);

    if (ctx->data_sock    >= 0) close(ctx->data_sock);
    if (ctx->control_sock >= 0) close(ctx->control_sock);

    if (ctx->decoder_ctx) {
        AVCodecContext *avctx = (AVCodecContext *)ctx->decoder_ctx;
        avcodec_free_context(&avctx);
        ctx->decoder_ctx = NULL;
    }
    if (ctx->decoder_frame) {
        AVFrame *frame = (AVFrame *)ctx->decoder_frame;
        av_frame_free(&frame);
        ctx->decoder_frame = NULL;
    }
    if (ctx->alsa_pcm) {
        snd_pcm_drain((snd_pcm_t *)ctx->alsa_pcm);
        snd_pcm_close((snd_pcm_t *)ctx->alsa_pcm);
        ctx->alsa_pcm = NULL;
    }

    pthread_mutex_destroy(&ctx->ring.lock);
    pthread_cond_destroy(&ctx->ring.cond);
}
