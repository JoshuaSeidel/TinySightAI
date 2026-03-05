#pragma once

/*
 * airplay_audio.h — AirPlay Audio Stream Handler
 *
 * Receives audio packets on the UDP port allocated during RTSP SETUP and
 * decodes + plays them via ALSA (Advanced Linux Sound Architecture).
 *
 * Audio formats used by AirPlay CarPlay:
 *
 *   AAC-LC  : MPEG-4 AAC Low Complexity (most common for CarPlay)
 *             RTP payload type 96 (dynamic)
 *             Sample rate: 44100 Hz, stereo
 *
 *   ALAC    : Apple Lossless Audio Codec
 *             RTP payload type 96 (dynamic)
 *             Sample rate: 44100 Hz, stereo
 *
 *   PCM     : Raw signed 16-bit, big-endian (less common)
 *             RTP payload type 96
 *
 * RTP packet format (RFC 3550):
 *   [2 bytes] V=2, P=0, X=0, CC=0, M=0, PT=96
 *   [2 bytes] sequence number (big-endian)
 *   [4 bytes] timestamp (big-endian, 44100 Hz clock)
 *   [4 bytes] SSRC (big-endian)
 *   [N bytes] audio payload
 *
 * We use libavcodec (from ffmpeg) for AAC/ALAC decoding.
 * Output is fed into an ALSA ring buffer.
 *
 * Ring buffer:
 *   ~100ms = 4410 samples @ 44100 Hz = 17640 bytes (stereo 16-bit)
 *   We keep ~200ms ahead to absorb network jitter.
 *
 * Volume:
 *   AirPlay volume is in dB, range -144 (mute) to 0 (max).
 *   We convert to a linear PCM scale factor.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/* Audio ring buffer size in bytes (~200ms at 44100 Hz stereo 16-bit) */
#define AUDIO_RING_BYTES   (44100 * 4 * 200 / 1000)   /* 35280 bytes */

/* ALSA device to output to */
#define ALSA_DEVICE        "default"

/* Sample rate */
#define AUDIO_SAMPLE_RATE  44100
#define AUDIO_CHANNELS     2
#define AUDIO_BITS         16

/* Audio encoding enum */
typedef enum {
    AUDIO_FMT_UNKNOWN = 0,
    AUDIO_FMT_PCM,
    AUDIO_FMT_AAC_LC,
    AUDIO_FMT_ALAC,
} audio_format_t;

/*
 * Audio ring buffer (lock-free single-producer, single-consumer).
 * Producer: network receive thread.
 * Consumer: ALSA playback thread.
 */
typedef struct {
    uint8_t  buf[AUDIO_RING_BYTES];
    volatile size_t write_pos;   /* next byte to write */
    volatile size_t read_pos;    /* next byte to read */
    pthread_mutex_t lock;
    pthread_cond_t  cond;        /* signals consumer that data is available */
} audio_ring_t;

/*
 * Per-connection audio context.
 */
typedef struct {
    /* UDP socket for RTP audio data */
    int data_sock;
    int control_sock;
    uint16_t data_port;
    uint16_t control_port;

    /* Audio format negotiated via SDP */
    audio_format_t format;

    /* Decoder state (ffmpeg AVCodecContext — opaque to keep header clean) */
    void *decoder_ctx;   /* AVCodecContext* */
    void *decoder_frame; /* AVFrame* */
    void *parser_ctx;    /* AVCodecParserContext* */

    /* ALSA PCM handle (opaque) */
    void *alsa_pcm;

    /* Ring buffer between decoder and ALSA */
    audio_ring_t ring;

    /* Volume: linear scale 0.0–1.0 */
    float volume;

    /* Threads */
    pthread_t recv_thread;
    pthread_t play_thread;
    volatile int running;

    /* Sequence tracking for reordering/loss detection */
    uint16_t last_seq;
    bool     first_packet;
} airplay_audio_ctx_t;

/*
 * Initialise audio context.
 * data_port and control_port must match what was returned in RTSP SETUP.
 * format is the codec agreed in SDP.
 */
int airplay_audio_ctx_init(airplay_audio_ctx_t *ctx,
                            uint16_t data_port,
                            uint16_t control_port,
                            audio_format_t format);

/*
 * Start receiving and playing audio.
 * Launches receive and playback threads.
 * Returns 0 on success.
 */
int airplay_audio_start(airplay_audio_ctx_t *ctx);

/*
 * Stop audio (signals threads to exit, joins them).
 */
void airplay_audio_stop(airplay_audio_ctx_t *ctx);

/*
 * Set volume (called from RTSP SET_PARAMETER).
 * vol_db is in dB, range [-144, 0]. -144 = mute, 0 = full.
 */
void airplay_audio_set_volume(float vol_db);

/*
 * Free all resources.
 */
void airplay_audio_ctx_destroy(airplay_audio_ctx_t *ctx);
