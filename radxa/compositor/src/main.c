/*
 * AADongle Compositor — Main service
 *
 * Architecture overview:
 *
 *   aa-proxy (Rust, port 5277/5288) is the AAP relay layer. It owns the TCP
 *   connections to the T-Dongle (car) and to the Android phone. The compositor
 *   is NOT in the AAP data path for AA traffic. Instead:
 *
 *   1. aa-proxy taps a COPY of every channel-3 (video) NAL payload it receives
 *      from the phone and writes it to /tmp/aa-video.sock (compositor listens).
 *
 *   2. The compositor decodes the tapped frame, composites with the camera feed,
 *      re-encodes to H.264, and sends the result to /tmp/aa-video-out.sock
 *      (aa-proxy listens there). aa-proxy substitutes the original channel-3
 *      NAL with the composited H.264 before forwarding to the car.
 *
 *   3. For CarPlay (iPhone via AirPlay), the AirPlay receiver sends H.264/H.265
 *      NAL frames to /tmp/carplay-video.sock (compositor listens). The
 *      compositor processes them and sends the result to aa-proxy via
 *      /tmp/aa-video-out.sock. aa-proxy's standalone mode injects these
 *      as AAP channel-3 frames into the car when no Android phone is present.
 *
 *   4. Camera-only mode (no phone): compositor encodes raw camera frames and
 *      sends them to aa-proxy via /tmp/aa-video-out.sock (same path as #3).
 *
 *   5. Control channel (TCP 5290 / /tmp/compositor-control.sock): mode changes,
 *      zoom, IR LED control. MODE commands are also forwarded to aa-proxy via
 *      /tmp/aa-control.sock so aa-proxy can update its touch remapping.
 *
 * Threads started here:
 *   aa_video_thread    — reads video taps from /tmp/aa-video.sock, composites,
 *                        writes composited H.264 to /tmp/aa-video-out.sock
 *   carplay_thread     — reads from /tmp/carplay-video.sock, composites, sends
 *                        to aa-proxy via /tmp/aa-video-out.sock (only active
 *                        when source == SOURCE_CARPLAY AND g_aa_active == 0)
 *   camera_only_thread — encodes raw camera frames, sends to aa-proxy via
 *                        /tmp/aa-video-out.sock when no phone video is active
 *   control_thread     — services control_poll() at ~100 Hz
 *
 * TCP ports 5277 and 5288 are owned exclusively by aa-proxy. The compositor
 * does NOT bind or connect to those ports.
 */

#include "pipeline.h"
#include "camera.h"
#include "mode.h"
#include "overlay.h"
#include "control_channel.h"
#include "carplay_input.h"
#include "nal_detect.h"
#include "aa_video_input.h"
#include "aa_video_output.h"
#include "ir_led.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#define OUTPUT_W    1280
#define OUTPUT_H    720
#define OUTPUT_FPS  30

/* ---- Global state ---- */

static pipeline_t      *g_pipeline = NULL;
static camera_t         g_camera;
static display_state_t  g_display;
static volatile int     g_running  = 1;

/*
 * g_aa_active: set to 1 while the aa-proxy video tap is connected and
 * delivering frames. carplay_thread and camera_only_thread check this flag
 * to avoid generating competing video output when AA video is running.
 * Written only by aa_video_thread; read by other threads without a lock
 * (volatile int is sufficient for this use-case: worst case we get a stale
 * read for one frame, which is harmless).
 */
static volatile int g_aa_active = 0;

/*
 * Single mutex serialising access to the hardware pipeline, display state,
 * and camera dequeue/enqueue. Held only during encode/decode/composite
 * operations — never across blocking I/O.
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Monotonic timestamp helper ---- */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- Signal handler ---- */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---- Video pipeline helper ---- */

/*
 * Decode `nal_data`, composite with camera (if layout requires it), and
 * encode to H.264.
 *
 * Returns a pointer to the encoded output and sets *out_len on success.
 * Returns NULL on decode/encode failure.
 *
 * Must be called with g_lock held.
 * The returned pointer is valid until the next call to pipeline_encode().
 */
static const uint8_t *process_video_frame(
    const uint8_t *nal_data, size_t nal_len,
    input_codec_t codec,
    uint64_t ts,
    size_t *out_len)
{
    if (pipeline_decode(g_pipeline, nal_data, nal_len, codec) < 0) {
        *out_len = 0;
        return NULL;
    }

    uint8_t *cam_data = NULL;
    size_t   cam_len  = 0;
    int      cam_idx  = -1;

    if (g_display.layout != LAYOUT_FULL_PRIMARY) {
        cam_idx = camera_dequeue(&g_camera, &cam_data, &cam_len);
        if (cam_data)
            ir_led_update(cam_data, g_camera.width, g_camera.height);
    }

    pipeline_composite(g_pipeline, g_display.layout,
                       cam_data, g_camera.width, g_camera.height);

    const uint8_t *encoded = pipeline_encode(g_pipeline, out_len);

    if (cam_idx >= 0)
        camera_enqueue(&g_camera, cam_idx);

    (void)ts; /* timestamp is used by caller when building the AAP wrapper */
    return encoded;
}

/* ---- AA video thread: tap from aa-proxy → composite → output ---- */

/*
 * Waits for aa-proxy to connect to /tmp/aa-video.sock and deliver video
 * tap frames. For each frame: decode → composite with camera → encode →
 * send composited H.264 to /tmp/aa-video-out.sock.
 *
 * aa-proxy reads from /tmp/aa-video-out.sock and injects the composited
 * output as the car-bound channel-3 payload instead of the original NAL.
 *
 * Sets g_aa_active = 1 while frames are arriving; resets it to 0 when idle
 * so carplay_thread / camera_only_thread know they can generate output.
 */
static void *aa_video_thread(void *arg)
{
    (void)arg;

    while (g_running) {
        const uint8_t *nal  = NULL;
        size_t         nlen = 0;
        input_codec_t  codec;

        if (aa_video_input_get_frame(&nal, &nlen, &codec) < 0) {
            if (g_aa_active) {
                g_aa_active = 0;
                printf("aa_video: tap idle — CarPlay/camera threads may run\n");
            }
            usleep(5000); /* poll at 200 Hz when idle */
            continue;
        }

        if (!g_aa_active) {
            g_aa_active = 1;
            printf("aa_video: tap active — AA video compositing started\n");
        }

        pthread_mutex_lock(&g_lock);

        uint64_t ts = monotonic_ns();
        size_t out_len = 0;
        const uint8_t *h264_out = process_video_frame(
            nal, nlen, codec, ts, &out_len);

        pthread_mutex_unlock(&g_lock);

        if (h264_out && out_len > 0) {
            /*
             * Send composited H.264 back to aa-proxy via the output socket.
             * aa-proxy will inject this in place of the original phone video
             * into the car-bound channel-3 AAP frame.
             */
            aa_video_output_send_frame(h264_out, out_len);
        }
    }

    g_aa_active = 0;
    return NULL;
}

/* ---- CarPlay thread: AirPlay receiver → composite → car ---- */

/*
 * When the user has an iPhone connected (CarPlay mode) and AA video is not
 * active, this thread reads frames from the AirPlay receiver via
 * /tmp/carplay-video.sock, composites them with the camera feed, and
 * sends the result to aa-proxy via /tmp/aa-video-out.sock.
 *
 * aa-proxy's standalone mode reads from this socket and injects the
 * composited H.264 as AAP channel-3 video frames into the car.
 */
static void *carplay_thread(void *arg)
{
    (void)arg;

    while (g_running) {
        /*
         * Yield when AA video is active (aa_video_thread is handling all
         * compositing and delivering output to aa-proxy which forwards to the
         * car), or when the user has not selected CarPlay as the source.
         */
        if (g_aa_active || g_display.source != SOURCE_CARPLAY) {
            usleep(50000);
            continue;
        }

        const uint8_t *nal  = NULL;
        size_t         nlen = 0;
        input_codec_t  codec;

        if (carplay_input_get_frame(&nal, &nlen, &codec) < 0) {
            usleep(5000);
            continue;
        }

        pthread_mutex_lock(&g_lock);

        uint64_t ts = monotonic_ns();
        size_t out_len = 0;
        const uint8_t *h264_out = process_video_frame(
            nal, nlen, codec, ts, &out_len);

        pthread_mutex_unlock(&g_lock);

        if (h264_out && out_len > 0) {
            aa_video_output_send_frame(h264_out, out_len);
        }
    }
    return NULL;
}

/* ---- Camera-only thread: no phone video, just baby cam ---- */

/*
 * When no AA or CarPlay video is active and the layout requires camera
 * output (LAYOUT_FULL_CAMERA or LAYOUT_SPLIT_LEFT_RIGHT), this thread
 * encodes raw camera NV12 frames and sends them to aa-proxy via
 * /tmp/aa-video-out.sock for injection into the car.
 */
static void *camera_only_thread(void *arg)
{
    (void)arg;

    while (g_running) {
        /*
         * Yield when:
         *   - AA video tap is active (aa_video_thread is compositing)
         *   - CarPlay is the source (carplay_thread handles compositing)
         *   - Layout is full-primary (primary source fills the screen;
         *     camera is not visible so nothing to encode here)
         */
        if (g_aa_active ||
            g_display.source == SOURCE_CARPLAY ||
            g_display.layout == LAYOUT_FULL_PRIMARY) {
            usleep(100000);
            continue;
        }

        uint8_t *cam_data = NULL;
        size_t   cam_len  = 0;
        int cam_idx = camera_dequeue(&g_camera, &cam_data, &cam_len);
        if (cam_idx < 0) {
            usleep(10000);
            continue;
        }

        ir_led_update(cam_data, g_camera.width, g_camera.height);

        pthread_mutex_lock(&g_lock);

        uint64_t ts = monotonic_ns();
        size_t out_len = 0;
        const uint8_t *h264_out = pipeline_encode_camera_only(
            g_pipeline, cam_data, g_camera.width, g_camera.height, &out_len);

        camera_enqueue(&g_camera, cam_idx);
        pthread_mutex_unlock(&g_lock);

        if (h264_out && out_len > 0) {
            aa_video_output_send_frame(h264_out, out_len);
        }
    }
    return NULL;
}

/* ---- Control channel thread ---- */

static void *control_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        control_poll();
        usleep(10000); /* ~100 Hz — low latency without busy-wait */
    }
    return NULL;
}

/* ---- IR LED control ---- */

/* GPIO3_D4 = (3*32) + (3*8) + 4 = 124 on Radxa Zero 3W */
#define IR_LED_GPIO 124

/*
 * Control channel callback: translate string commands to ir_led API.
 */
static void ir_control(const char *mode)
{
    if (strcmp(mode, "on") == 0)
        ir_led_set_mode(IR_MODE_ON);
    else if (strcmp(mode, "off") == 0)
        ir_led_set_mode(IR_MODE_OFF);
    else if (strcmp(mode, "auto") == 0)
        ir_led_set_mode(IR_MODE_AUTO);
    else
        printf("compositor: unknown IR mode '%s'\n", mode);
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    printf("=== AADongle Compositor v2.0 ===\n");
    printf("  AA relay:       aa-proxy (Rust) owns ports 5277/5288\n");
    printf("  AA video tap:   %s  (compositor server)\n", AA_VIDEO_SOCK_PATH);
    printf("  AA video out:   %s  (aa-proxy server)\n", AA_VIDEO_OUT_SOCK_PATH);
    printf("  CarPlay video:  %s  (compositor server)\n", CARPLAY_SOCK_PATH);
    printf("  Control:        TCP %d  |  %s\n",
           CONTROL_TCP_PORT, CONTROL_SOCK_PATH);
    printf("\n");

    /* Initialise display mode */
    mode_init(&g_display);

    /* Initialise overlay icons */
    if (overlay_init() < 0)
        fprintf(stderr, "WARNING: overlay init failed\n");

    /* Initialise camera */
    const char *cam_device = "/dev/video0";
    if (camera_init(&g_camera, cam_device) < 0) {
        fprintf(stderr, "WARNING: camera init failed — running without camera\n");
    } else {
        camera_start(&g_camera);
        printf("Camera started: %dx%d\n", g_camera.width, g_camera.height);
    }

    /* Initialise hardware video pipeline */
    g_pipeline = pipeline_init(OUTPUT_W, OUTPUT_H, OUTPUT_FPS);
    if (!g_pipeline) {
        fprintf(stderr, "ERROR: pipeline init failed\n");
        return 1;
    }

    /*
     * Initialise AA video tap input.
     * Compositor listens on /tmp/aa-video.sock; aa-proxy connects as client
     * and sends a copy of every channel-3 NAL it receives from the phone.
     */
    if (aa_video_input_init() < 0)
        fprintf(stderr, "WARNING: AA video input init failed\n");

    /*
     * Initialise AA video composited output.
     * Starts a background reconnect thread that connects to aa-proxy's
     * /tmp/aa-video-out.sock listener. Frames sent here replace the
     * original phone video in the car-bound AAP channel-3 stream.
     */
    if (aa_video_output_init() < 0)
        fprintf(stderr, "WARNING: AA video output init failed\n");

    /*
     * Initialise CarPlay input receiver.
     * Compositor listens on /tmp/carplay-video.sock; the AirPlay receiver
     * (RPiPlay/shairplay derivative) connects and sends H.264/H.265 NALs.
     */
    if (carplay_input_init() < 0)
        fprintf(stderr, "WARNING: CarPlay input init failed\n");

    /* Initialise IR LED GPIO (non-fatal if GPIO not available) */
    if (ir_led_init(IR_LED_GPIO) < 0)
        fprintf(stderr, "WARNING: IR LED GPIO init failed — dry-run mode\n");

    /*
     * Initialise control channel.
     * Listens on TCP 5290 and /tmp/compositor-control.sock.
     * MODE commands are forwarded to /tmp/aa-control.sock (aa-proxy) by
     * control_channel.c so aa-proxy can update its touch-remapping zones.
     */
    if (control_init(&g_display, &g_camera, &g_lock, ir_control) < 0)
        fprintf(stderr, "WARNING: control channel init failed\n");

    /*
     * CarPlay / camera-only video output path (Option A):
     *
     * All video output (AA composited, CarPlay, camera-only) flows through
     * /tmp/aa-video-out.sock → aa-proxy → car. aa-proxy's standalone mode
     * handles the case where no Android phone is connected: it reads
     * composited frames from CompositorOutput and injects them as AAP
     * channel-3 video into the car-bound TCP stream.
     *
     * The AA emulator (aa_emulator.c) is no longer used for video injection.
     */

    /* Launch worker threads */
    pthread_t t_aa_video, t_carplay, t_camera, t_control;

    if (pthread_create(&t_aa_video, NULL, aa_video_thread,    NULL) != 0) {
        perror("ERROR: pthread_create(aa_video_thread)");
        return 1;
    }
    if (pthread_create(&t_carplay,  NULL, carplay_thread,     NULL) != 0) {
        perror("ERROR: pthread_create(carplay_thread)");
        return 1;
    }
    if (pthread_create(&t_camera,   NULL, camera_only_thread, NULL) != 0) {
        perror("ERROR: pthread_create(camera_only_thread)");
        return 1;
    }
    if (pthread_create(&t_control,  NULL, control_thread,     NULL) != 0) {
        perror("ERROR: pthread_create(control_thread)");
        return 1;
    }

    printf("Compositor running. Waiting for connections...\n");

    /* Main loop — all work is done in threads */
    while (g_running)
        sleep(1);

    printf("Shutting down...\n");
    g_running = 0;

    /*
     * Destroy input/output modules first: this closes server fds and wakes
     * any threads blocked in accept() or recv(), allowing them to exit.
     */
    aa_video_input_destroy();
    aa_video_output_destroy();
    carplay_input_destroy();

    /* Wait for all worker threads to exit */
    pthread_join(t_aa_video, NULL);
    pthread_join(t_carplay,  NULL);
    pthread_join(t_camera,   NULL);
    pthread_join(t_control,  NULL);

    /* Tear down remaining resources */
    control_destroy();
    ir_led_destroy();
    pipeline_destroy(g_pipeline);
    camera_close(&g_camera);
    overlay_destroy();

    printf("Compositor stopped.\n");
    return 0;
}
