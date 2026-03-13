/*
 * baby_ai.c — AI Baby Monitoring via Allwinner A733 NPU
 *
 * Uses the awnn wrapper library (over VeriSilicon VIPLite) to run YOLOv5s
 * object detection on camera frames via the VIP9000 NPU (3 TOPS @ INT8).
 * A dedicated inference thread runs at ~1 fps, independent of the
 * 30 fps video pipeline.
 *
 * Build requires: libawnn.so (built from ZIFENG278/ai-sdk during setup.sh)
 *   - awnn wraps VIPLite and handles buffer creation, dequantization
 *   - Kernel driver: /dev/vipcore (Allwinner vendor kernel)
 *   - Model: YOLOv5s INT8 NBG (.nb) file from ai-sdk examples
 *
 * Model: examples/yolov5/model/v2/yolov5.nb from ZIFENG278/ai-sdk
 *   - Input: 640x640x3 UINT8 NCHW (channel-first RGB, 0-255)
 *   - Output: 3 heads (P8/P16/P32), each [1, 3, H, W, 85]
 *     H/W = 80/40/20, anchors = 3 per head, 85 = 4 box + 1 obj + 80 classes
 *   - COCO class 0 = person (used for baby detection)
 */

#include "baby_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

/* awnn library — conditionally included.
 * If building without awnn/VIPLite, define BABY_AI_STUB for no-op stub. */
#ifndef BABY_AI_STUB
#include <awnn_lib.h>
#endif

#define DEFAULT_MODEL_PATH "/opt/aadongle/models/baby_detect.nb"
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

/* YOLOv5 anchor definitions (standard COCO anchors) */
#define NUM_HEADS    3
#define NUM_ANCHORS  3
#define NUM_CLASSES  80
#define NUM_ATTRS    (4 + 1 + NUM_CLASSES)  /* x,y,w,h + objectness + classes */

static const float ANCHORS[NUM_HEADS][NUM_ANCHORS][2] = {
    {{10, 13}, {16, 30},  {33, 23}},     /* P3/8  — 80x80 grid */
    {{30, 61}, {62, 45},  {59, 119}},    /* P4/16 — 40x40 grid */
    {{116, 90}, {156, 198}, {373, 326}}  /* P5/32 — 20x20 grid */
};
static const int STRIDES[NUM_HEADS] = {8, 16, 32};
static const int GRID_SIZES[NUM_HEADS] = {80, 40, 20};

/* ---- Internal state ---- */

typedef struct {
#ifndef BABY_AI_STUB
    Awnn_Context_t *ctx;
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

    /* NCHW conversion buffer for model input (3 * 640 * 640) */
    uint8_t        *nchw_buf;
} baby_ai_ctx_t;

static baby_ai_ctx_t g_ai;

/* ---- Helpers ---- */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

/*
 * Convert NV12 to NCHW uint8 RGB with resize to MODEL_INPUT_W x MODEL_INPUT_H.
 * Output layout: [R plane][G plane][B plane], each 640*640 bytes.
 * Uses nearest-neighbor sampling for speed (AI model doesn't need bilinear).
 * No normalization — model handles 1/255 scaling internally.
 */
static void nv12_to_nchw_resize(const uint8_t *nv12, int src_w, int src_h,
                                 uint8_t *nchw, int dst_w, int dst_h)
{
    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + src_w * src_h;
    int plane_size = dst_w * dst_h;

    uint8_t *r_plane = nchw;
    uint8_t *g_plane = nchw + plane_size;
    uint8_t *b_plane = nchw + plane_size * 2;

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

            int idx = dy * dst_w + dx;
            r_plane[idx] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            g_plane[idx] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            b_plane[idx] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
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

/* ---- NPU inference via awnn ---- */

#ifndef BABY_AI_STUB

static int load_model(const char *path)
{
    awnn_init();

    g_ai.ctx = awnn_create(path);
    if (!g_ai.ctx) {
        fprintf(stderr, "baby_ai: awnn_create failed for %s\n", path);
        awnn_uninit();
        return -1;
    }

    printf("baby_ai: model loaded via awnn (VIP9000 NPU): %s\n", path);
    g_ai.model_loaded = true;
    return 0;
}

/*
 * Parse YOLOv5 output head for person detections.
 *
 * Each head has shape [1, 3, grid_h, grid_w, 85] (auto-dequantized to float32).
 * Layout in memory (row-major): anchor, row, col, attr.
 *
 * Updates best_* if a higher-confidence person detection is found.
 */
static void parse_yolov5_head(const float *data, int head_idx,
                               float *best_conf,
                               float *best_x, float *best_y,
                               float *best_w, float *best_h)
{
    int grid_h = GRID_SIZES[head_idx];
    int grid_w = GRID_SIZES[head_idx];
    int stride = STRIDES[head_idx];

    for (int a = 0; a < NUM_ANCHORS; a++) {
        float anchor_w = ANCHORS[head_idx][a][0];
        float anchor_h = ANCHORS[head_idx][a][1];

        for (int row = 0; row < grid_h; row++) {
            for (int col = 0; col < grid_w; col++) {
                int offset = ((a * grid_h + row) * grid_w + col) * NUM_ATTRS;

                float obj = sigmoid(data[offset + 4]);
                if (obj < 0.25f)
                    continue;  /* skip low-objectness early */

                float person_score = sigmoid(data[offset + 5 + COCO_PERSON]);
                float final_score = obj * person_score;

                if (final_score > CONF_THRESHOLD && final_score > *best_conf) {
                    /* Decode box coordinates */
                    float bx = (sigmoid(data[offset + 0]) * 2.0f - 0.5f + col) * stride;
                    float by = (sigmoid(data[offset + 1]) * 2.0f - 0.5f + row) * stride;
                    float bw = powf(sigmoid(data[offset + 2]) * 2.0f, 2) * anchor_w;
                    float bh = powf(sigmoid(data[offset + 3]) * 2.0f, 2) * anchor_h;

                    *best_conf = final_score;
                    *best_x = bx;
                    *best_y = by;
                    *best_w = bw;
                    *best_h = bh;
                }
            }
        }
    }
}

/*
 * Run YOLOv5s inference on an NCHW uint8 image.
 * Parses 3 output heads for person detections.
 */
static void run_inference(const uint8_t *nchw, int w, int h,
                           baby_ai_status_t *result)
{
    (void)w; (void)h;  /* always MODEL_INPUT_W x MODEL_INPUT_H */

    if (!g_ai.model_loaded || !g_ai.ctx) return;

    /* Feed input — awnn expects void* array of input buffers */
    void *input_bufs[1] = { (void *)nchw };
    awnn_set_input_buffers(g_ai.ctx, input_bufs);

    /* Run inference on NPU */
    awnn_run(g_ai.ctx);

    /* Get dequantized float32 output buffers (3 heads) */
    float **outputs = awnn_get_output_buffers(g_ai.ctx);
    if (!outputs) {
        fprintf(stderr, "baby_ai: awnn_get_output_buffers returned NULL\n");
        return;
    }

    /* Parse all 3 YOLOv5 heads for best person detection */
    float best_conf = 0.0f;
    float best_x = 0, best_y = 0, best_w = 0, best_h = 0;

    for (int i = 0; i < NUM_HEADS; i++) {
        if (outputs[i]) {
            parse_yolov5_head(outputs[i], i,
                              &best_conf, &best_x, &best_y, &best_w, &best_h);
        }
    }

    /* Update result */
    if (best_conf > CONF_THRESHOLD) {
        result->confidence = best_conf;
        /* Convert from model coords (0-640) to bounding box */
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
}

#else /* BABY_AI_STUB */

static int load_model(const char *path)
{
    (void)path;
    printf("baby_ai: STUB mode — no awnn/VIPLite runtime, AI features disabled\n");
    return -1;
}

static void run_inference(const uint8_t *nchw, int w, int h,
                           baby_ai_status_t *result)
{
    (void)nchw; (void)w; (void)h; (void)result;
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

        /* Convert NV12 to NCHW RGB and resize for model */
        nv12_to_nchw_resize(g_ai.frame_buf, w, h,
                            g_ai.nchw_buf, MODEL_INPUT_W, MODEL_INPUT_H);
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

        run_inference(g_ai.nchw_buf, MODEL_INPUT_W, MODEL_INPUT_H, &result);
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

    /* Allocate NCHW buffer for model input (3 planes of 640x640) */
    g_ai.nchw_buf = malloc(MODEL_INPUT_W * MODEL_INPUT_H * MODEL_INPUT_CH);
    if (!g_ai.nchw_buf) {
        fprintf(stderr, "baby_ai: NCHW buffer alloc failed\n");
        free(g_ai.frame_buf);
        return -1;
    }

    /* Load NBG model via awnn */
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
    if (g_ai.ctx) {
        awnn_destroy(g_ai.ctx);
        g_ai.ctx = NULL;
    }
    awnn_uninit();
#endif
    g_ai.model_loaded = false;

    free(g_ai.frame_buf);
    free(g_ai.nchw_buf);
    free(g_ai.prev_y);

    pthread_mutex_destroy(&g_ai.frame_lock);
    pthread_mutex_destroy(&g_ai.status_lock);

    printf("baby_ai: destroyed\n");
}
