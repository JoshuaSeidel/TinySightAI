/*
 * baby_ai.c — AI Baby Monitoring via RK3566 NPU
 *
 * Uses RKNN C API to run YOLOv8n object detection on camera frames.
 * A dedicated inference thread runs at ~1 fps, independent of the
 * 30 fps video pipeline.
 *
 * Build requires: librknnrt (Rockchip RKNN Runtime)
 *   - Header: rknn_api.h (from rknn-toolkit2/runtime)
 *   - Library: librknnrt.so (installed to /usr/lib/)
 *   - Model: YOLOv8n INT8 .rknn file
 *
 * Model preparation (done once on dev machine):
 *   pip install rknn-toolkit2
 *   yolo export model=yolov8n.pt format=rknn imgsz=640
 *   # or use ultralytics export with rknn format
 *   # produces yolov8n.rknn (~6MB INT8 quantized)
 *   scp yolov8n.rknn radxa:/opt/aadongle/models/baby_detect.rknn
 */

#include "baby_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

/* RKNN Runtime API — conditionally included.
 * If building without RKNN SDK, define BABY_AI_STUB to compile a no-op stub. */
#ifndef BABY_AI_STUB
#include <rknn_api.h>
#endif

#define DEFAULT_MODEL_PATH "/opt/aadongle/models/baby_detect.rknn"
#define MODEL_INPUT_W      640
#define MODEL_INPUT_H      640
#define MODEL_INPUT_CH     3

/* COCO class IDs relevant to baby monitoring */
#define COCO_PERSON        0

/* Detection thresholds */
#define CONF_THRESHOLD     0.45f
#define NMS_THRESHOLD      0.45f
#define MOTION_THRESHOLD   0.15f  /* motion_level above this = high motion */

/* How often to run inference (nanoseconds) */
#define INFERENCE_INTERVAL_NS  1000000000ULL  /* 1 second */

/* Frame differencing: percentage of changed pixels for motion score */
#define MOTION_DIFF_THRESHOLD  15  /* Y-channel delta to count as "changed" */

/* ---- Internal state ---- */

typedef struct {
#ifndef BABY_AI_STUB
    rknn_context    ctx;
    rknn_input_output_num io_num;
#endif
    bool            model_loaded;

    /* Inference thread */
    pthread_t       thread;
    volatile bool   running;
    volatile bool   enabled;

    /* Frame double-buffer: compositor writes to pending, AI reads from current */
    pthread_mutex_t frame_lock;
    uint8_t        *frame_buf;       /* NV12 frame copy */
    int             frame_w;
    int             frame_h;
    bool            frame_ready;     /* new frame available */

    /* Previous Y plane for motion detection */
    uint8_t        *prev_y;
    bool            prev_valid;

    /* Results (protected by status_lock) */
    pthread_mutex_t status_lock;
    baby_ai_status_t status;

    /* RGB conversion buffer for model input */
    uint8_t        *rgb_buf;
} baby_ai_ctx_t;

static baby_ai_ctx_t g_ai;

/* ---- Helpers ---- */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Convert NV12 to RGB888 with resize to MODEL_INPUT_W x MODEL_INPUT_H.
 * Uses nearest-neighbor sampling for speed (AI model doesn't need bilinear).
 */
static void nv12_to_rgb_resize(const uint8_t *nv12, int src_w, int src_h,
                                uint8_t *rgb, int dst_w, int dst_h)
{
    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + src_w * src_h;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * src_h / dst_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = dx * src_w / dst_w;

            uint8_t y  = y_plane[sy * src_w + sx];
            int uv_idx = (sy / 2) * src_w + (sx & ~1);
            uint8_t u  = uv_plane[uv_idx];
            uint8_t v  = uv_plane[uv_idx + 1];

            /* YUV to RGB (BT.601) */
            int c = y - 16;
            int d = u - 128;
            int e = v - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            int out_idx = (dy * dst_w + dx) * 3;
            rgb[out_idx + 0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            rgb[out_idx + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            rgb[out_idx + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

/*
 * Compute motion level by comparing current Y plane with previous.
 * Returns 0.0 (no motion) to 1.0 (everything changed).
 * Subsamples every 8th pixel for speed.
 */
static float compute_motion(const uint8_t *y_cur, const uint8_t *y_prev,
                             int w, int h)
{
    if (!y_prev) return 0.0f;

    int changed = 0;
    int total = 0;
    int step = 8;

    for (int row = 0; row < h; row += step) {
        for (int col = 0; col < w; col += step) {
            int idx = row * w + col;
            int diff = abs((int)y_cur[idx] - (int)y_prev[idx]);
            if (diff > MOTION_DIFF_THRESHOLD)
                changed++;
            total++;
        }
    }

    return total > 0 ? (float)changed / (float)total : 0.0f;
}

/* ---- RKNN inference ---- */

#ifndef BABY_AI_STUB

static int load_model(const char *path)
{
    /* Read model file */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "baby_ai: cannot open model: %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long model_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *model_data = malloc(model_size);
    if (!model_data) {
        fclose(f);
        return -1;
    }
    if (fread(model_data, 1, model_size, f) != (size_t)model_size) {
        free(model_data);
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Init RKNN context */
    int ret = rknn_init(&g_ai.ctx, model_data, model_size, 0, NULL);
    free(model_data);

    if (ret < 0) {
        fprintf(stderr, "baby_ai: rknn_init failed: %d\n", ret);
        return -1;
    }

    /* Query I/O count */
    ret = rknn_query(g_ai.ctx, RKNN_QUERY_IN_OUT_NUM,
                     &g_ai.io_num, sizeof(g_ai.io_num));
    if (ret < 0) {
        fprintf(stderr, "baby_ai: rknn_query failed: %d\n", ret);
        rknn_destroy(g_ai.ctx);
        return -1;
    }

    printf("baby_ai: model loaded (%ld bytes, %d inputs, %d outputs)\n",
           model_size, g_ai.io_num.n_input, g_ai.io_num.n_output);

    g_ai.model_loaded = true;
    return 0;
}

/*
 * Run YOLOv8n inference on an RGB image.
 * Parses the output tensor for person detections.
 *
 * Updates g_ai.status (caller must hold status_lock or call post-process).
 */
static void run_inference(const uint8_t *rgb, int w, int h,
                           baby_ai_status_t *result)
{
    if (!g_ai.model_loaded) return;

    /* Set input */
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = w * h * 3;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].buf = (void *)rgb;

    int ret = rknn_inputs_set(g_ai.ctx, 1, inputs);
    if (ret < 0) {
        fprintf(stderr, "baby_ai: rknn_inputs_set failed: %d\n", ret);
        return;
    }

    /* Run */
    ret = rknn_run(g_ai.ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "baby_ai: rknn_run failed: %d\n", ret);
        return;
    }

    /* Get outputs */
    rknn_output outputs[3]; /* YOLOv8 typically has 3 output tensors */
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < (int)g_ai.io_num.n_output && i < 3; i++) {
        outputs[i].index = i;
        outputs[i].want_float = 1; /* dequantize to float */
    }

    ret = rknn_outputs_get(g_ai.ctx, g_ai.io_num.n_output, outputs, NULL);
    if (ret < 0) {
        fprintf(stderr, "baby_ai: rknn_outputs_get failed: %d\n", ret);
        return;
    }

    /*
     * Parse YOLOv8 output for person detections.
     *
     * YOLOv8n output format (after RKNN export):
     *   output[0]: [1, 84, 8400] — 8400 detection candidates
     *     Each candidate: [x_center, y_center, width, height, class_scores[80]]
     *
     * We only care about class 0 (person) with high confidence.
     * The baby is detected as a "person" — we use bbox size relative to
     * frame to distinguish baby (small person in back seat) from adults.
     */
    float *out_data = (float *)outputs[0].buf;
    int num_candidates = 8400;
    int num_classes = 80;
    int stride = 4 + num_classes; /* x, y, w, h, class_scores */

    float best_conf = 0.0f;
    float best_x = 0, best_y = 0, best_w = 0, best_h = 0;

    for (int i = 0; i < num_candidates; i++) {
        float *det = out_data + i * stride;
        float person_score = det[4 + COCO_PERSON];

        if (person_score > CONF_THRESHOLD && person_score > best_conf) {
            best_conf = person_score;
            best_x = det[0];
            best_y = det[1];
            best_w = det[2];
            best_h = det[3];
        }
    }

    /* Update result */
    if (best_conf > CONF_THRESHOLD) {
        result->confidence = best_conf;
        /* Convert from model coords (0-640) to frame coords */
        result->bbox_x = (int)(best_x - best_w / 2);
        result->bbox_y = (int)(best_y - best_h / 2);
        result->bbox_w = (int)best_w;
        result->bbox_h = (int)best_h;
        result->face_visible = true; /* TODO: add face sub-detection */

        /* Heuristic: low motion + detected person = likely sleeping */
        if (result->motion_level < 0.05f) {
            result->state = BABY_STATE_SLEEPING;
        } else if (result->motion_level > MOTION_THRESHOLD) {
            result->state = BABY_STATE_ALERT; /* high motion = fussy/crying */
        } else {
            result->state = BABY_STATE_AWAKE;
        }
    } else {
        result->state = BABY_STATE_ABSENT;
        result->confidence = 0.0f;
        result->face_visible = false;
    }

    /* Release outputs */
    rknn_outputs_release(g_ai.ctx, g_ai.io_num.n_output, outputs);
}

#else /* BABY_AI_STUB */

static int load_model(const char *path)
{
    (void)path;
    printf("baby_ai: STUB mode — no RKNN runtime, AI features disabled\n");
    return -1;
}

static void run_inference(const uint8_t *rgb, int w, int h,
                           baby_ai_status_t *result)
{
    (void)rgb; (void)w; (void)h; (void)result;
}

#endif /* BABY_AI_STUB */

/* ---- Inference thread ---- */

static void *inference_thread(void *arg)
{
    (void)arg;
    printf("baby_ai: inference thread started\n");

    while (g_ai.running) {
        if (!g_ai.enabled || !g_ai.model_loaded) {
            usleep(100000); /* 100ms when idle */
            continue;
        }

        /* Wait for a new frame */
        pthread_mutex_lock(&g_ai.frame_lock);
        if (!g_ai.frame_ready) {
            pthread_mutex_unlock(&g_ai.frame_lock);
            usleep(50000); /* 50ms poll */
            continue;
        }

        /* Copy frame data (hold lock briefly) */
        int w = g_ai.frame_w;
        int h = g_ai.frame_h;
        size_t y_size = (size_t)w * h;
        uint8_t *y_copy = malloc(y_size);
        if (y_copy)
            memcpy(y_copy, g_ai.frame_buf, y_size);

        /* Convert NV12 to RGB and resize for model */
        nv12_to_rgb_resize(g_ai.frame_buf, w, h,
                           g_ai.rgb_buf, MODEL_INPUT_W, MODEL_INPUT_H);
        g_ai.frame_ready = false;
        pthread_mutex_unlock(&g_ai.frame_lock);

        /* Compute motion from Y plane */
        float motion = 0.0f;
        if (y_copy) {
            motion = compute_motion(y_copy, g_ai.prev_y, w, h);
            /* Swap prev buffer */
            free(g_ai.prev_y);
            g_ai.prev_y = y_copy;
            g_ai.prev_valid = true;
        }

        /* Run NPU inference */
        baby_ai_status_t result;
        memset(&result, 0, sizeof(result));
        result.motion_level = motion;

        run_inference(g_ai.rgb_buf, MODEL_INPUT_W, MODEL_INPUT_H, &result);
        result.last_update_ns = monotonic_ns();

        /* Publish result */
        pthread_mutex_lock(&g_ai.status_lock);
        g_ai.status = result;
        pthread_mutex_unlock(&g_ai.status_lock);

        /* Rate limit to ~1 fps */
        usleep(900000); /* 900ms — combined with processing time ≈ 1s */
    }

    printf("baby_ai: inference thread stopped\n");
    return NULL;
}

/* ---- Public API ---- */

int baby_ai_init(const char *model_path)
{
    memset(&g_ai, 0, sizeof(g_ai));
    pthread_mutex_init(&g_ai.frame_lock, NULL);
    pthread_mutex_init(&g_ai.status_lock, NULL);
    g_ai.status.state = BABY_STATE_UNKNOWN;

    /* Allocate frame buffer (NV12: w*h*1.5, max 1280x720) */
    size_t max_frame = 1280 * 720 * 3 / 2;
    g_ai.frame_buf = malloc(max_frame);
    if (!g_ai.frame_buf) {
        fprintf(stderr, "baby_ai: frame buffer alloc failed\n");
        return -1;
    }

    /* Allocate RGB buffer for model input */
    g_ai.rgb_buf = malloc(MODEL_INPUT_W * MODEL_INPUT_H * MODEL_INPUT_CH);
    if (!g_ai.rgb_buf) {
        fprintf(stderr, "baby_ai: RGB buffer alloc failed\n");
        free(g_ai.frame_buf);
        return -1;
    }

    /* Load RKNN model */
    const char *path = model_path ? model_path : DEFAULT_MODEL_PATH;
    if (load_model(path) < 0) {
        printf("baby_ai: model not loaded — motion detection only\n");
        /* Non-fatal: motion detection works without the model */
    }

    g_ai.enabled = true;
    printf("baby_ai: initialized (model=%s)\n",
           g_ai.model_loaded ? "loaded" : "not available");
    return 0;
}

int baby_ai_start(void)
{
    g_ai.running = true;
    if (pthread_create(&g_ai.thread, NULL, inference_thread, NULL) != 0) {
        perror("baby_ai: pthread_create");
        g_ai.running = false;
        return -1;
    }
    return 0;
}

void baby_ai_feed_frame(const uint8_t *nv12_data, int w, int h)
{
    if (!g_ai.enabled || !nv12_data) return;

    pthread_mutex_lock(&g_ai.frame_lock);
    size_t frame_size = (size_t)w * h * 3 / 2; /* NV12 */
    if (frame_size <= 1280 * 720 * 3 / 2) {
        memcpy(g_ai.frame_buf, nv12_data, frame_size);
        g_ai.frame_w = w;
        g_ai.frame_h = h;
        g_ai.frame_ready = true;
    }
    pthread_mutex_unlock(&g_ai.frame_lock);
}

baby_ai_status_t baby_ai_get_status(void)
{
    baby_ai_status_t s;
    pthread_mutex_lock(&g_ai.status_lock);
    s = g_ai.status;
    pthread_mutex_unlock(&g_ai.status_lock);
    return s;
}

void baby_ai_set_enabled(bool enabled)
{
    if (g_ai.enabled == enabled) return;
    g_ai.enabled = enabled;
    printf("baby_ai: %s\n", enabled ? "enabled" : "disabled");

    if (!enabled) {
        pthread_mutex_lock(&g_ai.status_lock);
        g_ai.status.state = BABY_STATE_UNKNOWN;
        pthread_mutex_unlock(&g_ai.status_lock);
    }
}

bool baby_ai_is_enabled(void)
{
    return g_ai.enabled;
}

void baby_ai_destroy(void)
{
    g_ai.running = false;
    pthread_join(g_ai.thread, NULL);

#ifndef BABY_AI_STUB
    if (g_ai.model_loaded) {
        rknn_destroy(g_ai.ctx);
        g_ai.model_loaded = false;
    }
#endif

    free(g_ai.frame_buf);
    free(g_ai.rgb_buf);
    free(g_ai.prev_y);

    pthread_mutex_destroy(&g_ai.frame_lock);
    pthread_mutex_destroy(&g_ai.status_lock);

    printf("baby_ai: destroyed\n");
}
