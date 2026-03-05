#pragma once

/*
 * airplay_fairplay.h — FairPlay DRM Handshake for AirPlay Screen Mirroring
 *
 * AirPlay screen mirroring uses a FairPlay "satellite" handshake that is
 * documented via open-source research (RPiPlay, shairport-sync, etc.).  The
 * handshake is a 3-stage binary blob exchange over the RTSP /fp-setup endpoint.
 *
 * Overview of the 3 stages:
 *
 *   Stage 1: iPhone sends 16 bytes
 *            → Server returns 142 bytes (fixed blob with server ephemeral key)
 *
 *   Stage 2: iPhone sends 164 bytes (contains encrypted client key material)
 *            → Server decrypts client key, returns 32 bytes
 *
 *   Stage 3: iPhone sends 20 bytes (confirmation / final key material)
 *            → Server derives the AES-128 key used to encrypt mirror H.264 data
 *            → No response body needed (return empty 200 OK)
 *
 * After all 3 stages the caller retrieves the AES key via fairplay_get_aes_key()
 * and hands it to the mirror parser.
 *
 * The "well-known" constant bytes in the response blobs match those used by
 * RPiPlay (github.com/FD-/RPiPlay) and are derived from FairPlay specification
 * research; they are not secret.
 *
 * Reference: https://github.com/FD-/RPiPlay/blob/master/renderers/ap_crypto.c
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Maximum blob sizes (conservatively larger than needed) */
#define FP_STAGE1_REQ_LEN   16
#define FP_STAGE1_RESP_LEN  142
#define FP_STAGE2_REQ_LEN   164
#define FP_STAGE2_RESP_LEN  32
#define FP_STAGE3_REQ_LEN   20
/* Stage 3 returns empty body */

typedef enum {
    FP_STATE_INIT    = 0,
    FP_STATE_STAGE1  = 1,  /* stage 1 complete */
    FP_STATE_STAGE2  = 2,  /* stage 2 complete */
    FP_STATE_DONE    = 3,  /* AES key ready */
    FP_STATE_ERROR   = -1,
} fairplay_state_t;

typedef struct {
    fairplay_state_t state;

    /* AES-128 key derived at stage 3, used for mirror stream decryption */
    uint8_t aes_key[16];
    bool    aes_key_valid;

    /* Internal stage data */
    uint8_t stage1_req[FP_STAGE1_REQ_LEN];
    uint8_t stage2_req[FP_STAGE2_REQ_LEN];
    uint8_t stage3_req[FP_STAGE3_REQ_LEN];

    /* Server-side ephemeral data generated during stage 1 */
    uint8_t server_key[16];  /* random, generated at stage 1 */
    uint8_t server_iv[16];   /* random, generated at stage 1 */
} airplay_fairplay_ctx_t;

/*
 * Initialise a FairPlay context.
 * Must be called before any stage processing.
 */
int fairplay_ctx_init(airplay_fairplay_ctx_t *ctx);

/*
 * Process /fp-setup data.
 *
 *   in_data   : body of the POST request
 *   in_len    : body length
 *   out_data  : caller-supplied buffer (>= 256 bytes)
 *   out_len   : set to number of valid bytes written to out_data on success
 *
 * Returns 0 on success, -1 on error.
 * The stage is determined automatically from in_len.
 */
int fairplay_process(airplay_fairplay_ctx_t *ctx,
                     const uint8_t *in_data, size_t in_len,
                     uint8_t *out_data, size_t *out_len);

/*
 * After fairplay_process() returns FP_STATE_DONE, retrieve the derived AES key.
 * key must point to a 16-byte buffer.
 * Returns 0 if key is ready, -1 if handshake not yet complete.
 */
int fairplay_get_aes_key(const airplay_fairplay_ctx_t *ctx, uint8_t key[16]);

/*
 * After fairplay_process() returns FP_STATE_DONE, retrieve the AES-CTR IV.
 *
 * The IV is the server_iv[16] generated at stage 1 of the FairPlay handshake.
 * It is used as the initial counter value for AES-128-CTR decryption of the
 * mirror H.264 stream (bytes [0..11] of the 16-byte counter block; bytes
 * [12..15] are the per-block counter that increments automatically).
 *
 * iv must point to a 16-byte buffer.
 * Returns 0 if the IV is ready, -1 if handshake not yet complete.
 */
int fairplay_get_iv(const airplay_fairplay_ctx_t *ctx, uint8_t iv[16]);

/*
 * Current handshake state.
 */
fairplay_state_t fairplay_get_state(const airplay_fairplay_ctx_t *ctx);

/*
 * Free resources (nothing heap-allocated currently, but good practice).
 */
void fairplay_ctx_destroy(airplay_fairplay_ctx_t *ctx);
