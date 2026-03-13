#pragma once
/*
 * baby_ai.h — AI Baby Monitoring via Allwinner A733 NPU
 *
 * Uses the awnn wrapper library (over VIPLite) to run YOLOv5s object detection
 * on camera frames via the VIP9000 NPU (3 TOPS @ INT8 on Allwinner A733).
 * Detections run at ~1 fps in a dedicated thread, leaving the video pipeline
 * undisturbed.
 *
 * Detections:
 *   - Baby present / not present (object detection)
 *   - Baby sleeping / awake (pose + face classifier)
 *   - Face covered / obstructed
 *   - Excessive motion / distress (frame differencing)
 *
 * Model: YOLOv5s INT8 NBG from ZIFENG278/ai-sdk (pre-converted for VIP9000)
 *   Installed by setup.sh from: ai-sdk/examples/yolov5/model/v2/yolov5.nb
 *   Expected at: /opt/aadongle/models/baby_detect.nb
 *
 * Alert flow:
 *   baby_ai_process() → updates baby_ai_status_t
 *   compositor reads status → renders overlay indicator
 *   control channel → reports status in STATUS response
 *   web UI / phone app → polls STATUS for alert display
 */

#include <stdint.h>
#include <stdbool.h>

/* Detection result flags */
typedef enum {
    BABY_STATE_UNKNOWN   = 0,
    BABY_STATE_ABSENT    = 1,  /* no baby detected in frame */
    BABY_STATE_AWAKE     = 2,  /* baby detected, appears awake */
    BABY_STATE_SLEEPING  = 3,  /* baby detected, eyes closed / still */
    BABY_STATE_ALERT     = 4,  /* face covered, unusual posture, distress */
} baby_state_t;

typedef struct {
    baby_state_t state;
    float        confidence;    /* 0.0-1.0 detection confidence */
    float        motion_level;  /* 0.0-1.0 motion between frames */
    int          bbox_x;        /* baby bounding box (for overlay) */
    int          bbox_y;
    int          bbox_w;
    int          bbox_h;
    bool         face_visible;  /* true if face detected within bbox */
    uint64_t     last_update_ns;/* monotonic timestamp of last inference */
} baby_ai_status_t;

/**
 * Initialize the AI module.
 * Loads the NBG model file and prepares the VIPLite NPU context.
 *
 * model_path: path to .nb model file (NULL = default path)
 * Returns 0 on success, -1 on failure (non-fatal — AI features disabled).
 */
int baby_ai_init(const char *model_path);

/**
 * Start the AI inference thread.
 * Must be called after baby_ai_init().
 */
int baby_ai_start(void);

/**
 * Feed a camera frame for AI processing.
 * Called from the compositor's camera frame path.
 * The frame is copied to an internal buffer; this call does not block.
 *
 * nv12_data: NV12 frame from camera
 * w, h: frame dimensions
 */
void baby_ai_feed_frame(const uint8_t *nv12_data, int w, int h);

/**
 * Get the current AI detection status (thread-safe snapshot).
 */
baby_ai_status_t baby_ai_get_status(void);

/**
 * Enable or disable AI processing.
 * When disabled, the inference thread sleeps and no NPU resources are used.
 */
void baby_ai_set_enabled(bool enabled);

/**
 * Check if AI is currently enabled.
 */
bool baby_ai_is_enabled(void);

/**
 * Stop the inference thread and release NPU resources.
 */
void baby_ai_destroy(void);
