#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "../mfi/mfi_auth.h"
#include "iap2_session.h"

/*
 * iAP2 Authentication State Machine
 *
 * Drives MFi challenge/response authentication from the accessory side.
 * The iPhone initiates auth after identification is accepted; this module
 * tracks state and applies timeouts + retry logic.
 *
 * State diagram:
 *
 *   IDLE
 *     |  iap2_auth_start()
 *     v
 *   CERT_REQUESTED   <-- waiting for RequestAuthenticationCertificate
 *     |  0xAA00 received
 *     v
 *   CERT_SENT        <-- certificate written to session
 *     |  0xAA02 received
 *     v
 *   CHALLENGE_RECEIVED
 *     |  MFi chip signs
 *     v
 *   CHALLENGE_SIGNED <-- signature written to session
 *     |  0xAA04 received
 *     v
 *   AUTH_COMPLETE
 *
 *   Any step → ERROR on timeout (3 retries, 5 s each) or 0xAA05
 *
 * Message IDs (big-endian uint16_t in iAP2 payload):
 *   0xAA00 — RequestAuthenticationCertificate  (iPhone → us)
 *   0xAA01 — AuthenticationCertificate         (us → iPhone)
 *   0xAA02 — RequestAuthenticationChallengeResponse (iPhone → us)
 *   0xAA03 — AuthenticationChallengeResponse   (us → iPhone)
 *   0xAA04 — AuthenticationSucceeded           (iPhone → us)
 *   0xAA05 — AuthenticationFailed              (iPhone → us)
 */

/* Message IDs used exclusively by auth module */
#define IAP2_AUTH_MSG_CERT_REQUEST       0xAA00
#define IAP2_AUTH_MSG_CERT_RESPONSE      0xAA01
#define IAP2_AUTH_MSG_CHALLENGE_REQUEST  0xAA02
#define IAP2_AUTH_MSG_CHALLENGE_RESPONSE 0xAA03
#define IAP2_AUTH_MSG_SUCCEEDED          0xAA04
#define IAP2_AUTH_MSG_FAILED             0xAA05

#define IAP2_AUTH_TIMEOUT_SEC    5
#define IAP2_AUTH_MAX_RETRIES    3

typedef enum {
    IAP2_AUTH_IDLE = 0,
    IAP2_AUTH_CERT_REQUESTED,
    IAP2_AUTH_CERT_SENT,
    IAP2_AUTH_CHALLENGE_RECEIVED,
    IAP2_AUTH_CHALLENGE_SIGNED,
    IAP2_AUTH_COMPLETE,
    IAP2_AUTH_ERROR,
} iap2_auth_state_t;

typedef void (*iap2_auth_done_cb_t)(bool success, void *ctx);

typedef struct {
    iap2_auth_state_t  state;
    pthread_mutex_t    mutex;

    mfi_device_t       mfi;          /* I2C handle to MFi chip */
    iap2_session_t    *session;      /* session to send responses through */

    int                retry_count;
    time_t             last_msg_time;/* timestamp of last state transition */

    iap2_auth_done_cb_t done_cb;     /* called on success or final failure */
    void               *done_ctx;
} iap2_auth_t;

/**
 * Open the MFi chip and initialise the auth state machine.
 *
 * i2c_device: e.g. "/dev/i2c-3"
 * chip_addr:  e.g. 0x10
 *
 * done_cb is called (from the same thread that calls
 * iap2_auth_handle_message) when auth completes or permanently fails.
 *
 * Returns 0 on success, -1 on failure.
 */
int iap2_auth_init(iap2_auth_t *auth,
                   const char *i2c_device, int chip_addr,
                   iap2_auth_done_cb_t done_cb, void *done_ctx);

/**
 * Attach a session and transition from IDLE to CERT_REQUESTED.
 * Call this once identification is accepted.
 */
int iap2_auth_start(iap2_auth_t *auth, iap2_session_t *session);

/**
 * Drive the auth state machine with an incoming iAP2 message.
 * msg_id, data, len come directly from the session layer dispatcher.
 *
 * Returns 0 if the message was handled (even if it triggered an error),
 * -1 if msg_id is not an auth message (caller should handle it elsewhere).
 */
int iap2_auth_handle_message(iap2_auth_t *auth,
                              uint16_t msg_id,
                              const uint8_t *data, size_t len);

/**
 * Periodic tick — call this from the main loop (e.g. every 500 ms) to
 * enforce the 5-second timeout and trigger retries.
 */
void iap2_auth_tick(iap2_auth_t *auth);

/**
 * True if authentication has completed successfully.
 */
bool iap2_auth_is_complete(const iap2_auth_t *auth);

/**
 * True if authentication has permanently failed.
 */
bool iap2_auth_is_error(const iap2_auth_t *auth);

/**
 * Close the MFi chip and destroy the mutex.
 */
void iap2_auth_cleanup(iap2_auth_t *auth);
