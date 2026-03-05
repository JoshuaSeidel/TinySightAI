/*
 * control_channel.h — Control channel for runtime compositor commands
 *
 * Accepts commands from two sources simultaneously:
 *   1. TCP socket on port 5290 — used by T-Dongle BLE bridge and remote UI
 *   2. Unix domain socket at /tmp/compositor-control.sock — local processes
 *
 * Simple line-oriented text protocol (commands terminated by '\n'):
 *
 *   MODE cycle              — cycle display mode (Full AA → Split → Full Cam)
 *   MODE full_aa            — switch to Full Android Auto
 *   MODE full_carplay       — switch to Full CarPlay
 *   MODE full_camera        — switch to Full camera
 *   MODE split_aa_cam       — split: AA left, camera right
 *   MODE split_cp_cam       — split: CarPlay left, camera right
 *   ZOOM in                 — camera zoom in one step
 *   ZOOM out                — camera zoom out one step
 *   ZOOM reset              — camera zoom reset to 1.0×
 *   IR on                   — IR LEDs on (via GPIO)
 *   IR off                  — IR LEDs off
 *   IR auto                 — IR LEDs auto (ambient-light based, future)
 *   STATUS                  — reply: "mode zoom ir fps latency\n"
 *
 * All commands are applied through the mode.h and camera.h APIs.
 * Callbacks for IR control are registered at init time.
 *
 * Thread safety: control_poll() may be called from the main thread or a
 * dedicated control thread. All state modifications use g_lock (passed in
 * at init via a pointer) to serialise with the video pipeline threads.
 */
#pragma once

#include "mode.h"
#include "camera.h"
#include <pthread.h>

#define CONTROL_TCP_PORT   5290
#define CONTROL_SOCK_PATH  "/tmp/compositor-control.sock"
#define CONTROL_MAX_CLIENTS 8

/* Callback for IR LED control.
 * mode is one of "on", "off", or "auto".
 * The callback writes the mode string to /tmp/ir-mode for ir-led-control.sh.
 */
typedef void (*ir_control_cb)(const char *mode);

/**
 * Initialise the control channel.
 *
 * @param display  Shared display state (modified by MODE commands).
 * @param cam      Camera handle (modified by ZOOM commands).
 * @param lock     Mutex that guards display and cam; held while applying cmds.
 * @param ir_cb    Optional callback for IR LED control (may be NULL).
 *
 * Returns 0 on success, -1 on failure.
 */
int control_init(display_state_t *display, camera_t *cam,
                 pthread_mutex_t *lock, ir_control_cb ir_cb);

/**
 * Poll for and process any pending control commands.
 * Non-blocking: accepts new connections and reads available data.
 * Call this regularly from the main loop or a dedicated thread.
 */
void control_poll(void);

/**
 * Close all sockets and free resources.
 */
void control_destroy(void);
