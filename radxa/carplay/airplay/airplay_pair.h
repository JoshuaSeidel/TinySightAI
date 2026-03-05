#pragma once

/*
 * airplay_pair.h — AirPlay Pair-Setup and Pair-Verify
 *
 * AirPlay 2 uses a HomeKit-derived pairing protocol:
 *
 *   /pair-setup  — SRP-6a key exchange + Ed25519 key registration
 *   /pair-verify — X25519 ECDH + Ed25519 signature to authenticate each session
 *
 * For CarPlay accessories the "transient" pairing mode is used:
 *   - No PIN entry required
 *   - SRP verifier uses an empty/fixed password ("3939" per the HAP spec)
 *   - The server auto-accepts any iPhone
 *
 * Pair-setup steps (POST /pair-setup, binary plist body):
 *   Step 1 (M1/M2):
 *     iPhone sends: { "pk": <client_SRP_A_bytes>, "proof": <client_M1_proof> }
 *     Server responds: { "pk": <server_SRP_B_bytes>, "proof": <server_M2_proof>,
 *                        "salt": <SRP_salt> }
 *   Step 2 (M3/M4 + key exchange):
 *     iPhone sends: { "epk": <encrypted Ed25519 pub key>, "authTag": <AES-GCM tag> }
 *     Server responds: { "epk": <server_encrypted_Ed25519_pub>, "authTag": <tag> }
 *
 * Pair-verify steps (POST /pair-verify, binary plist body):
 *   Step 1 (M1/M2):
 *     iPhone sends: { "pk": <client_X25519_pub (32B)>,
 *                     "data": <client_Ed25519_signature_over_shared_info> }
 *     Server generates X25519 keypair, derives shared secret,
 *     verifies client signature (or auto-accepts in transient mode).
 *     Server responds: { "pk": <server_X25519_pub>,
 *                        "data": <server_Ed25519_signature_encrypted_with_session_key> }
 *   Step 2 (M3/M4):
 *     iPhone sends: { "data": <verification_tag> }
 *     Server responds: {} (empty — verify success)
 *
 * After pair-verify the RTSP session has a shared 32-byte session key used
 * to derive per-connection AES-GCM encryption keys for control messages.
 *
 * Crypto primitives used:
 *   SRP-6a   : OpenSSL EVP_PKEY_CTX / SRP functions (or manual Diffie-Hellman)
 *   Ed25519  : OpenSSL EVP_PKEY (EVP_PKEY_ED25519)
 *   X25519   : OpenSSL EVP_PKEY (EVP_PKEY_X25519)
 *   HKDF     : OpenSSL EVP_PKEY_derive with EVP_PKEY_HKDF
 *   AES-GCM  : OpenSSL EVP_aes_128_gcm / EVP_aes_256_gcm
 *   ChaCha20 : OpenSSL EVP_chacha20_poly1305 (used for HAP encrypted records)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* HAP TLV8 type codes used in pair-setup/verify plist values */
#define HAP_TLV_METHOD        0x00
#define HAP_TLV_IDENTIFIER    0x01
#define HAP_TLV_SALT          0x02
#define HAP_TLV_PUBLIC_KEY    0x03
#define HAP_TLV_PROOF         0x04
#define HAP_TLV_ENCRYPTED_DATA 0x05
#define HAP_TLV_STATE         0x06
#define HAP_TLV_ERROR         0x07
#define HAP_TLV_RETRY_DELAY   0x08
#define HAP_TLV_CERTIFICATE   0x09
#define HAP_TLV_SIGNATURE     0x0A
#define HAP_TLV_PERMISSIONS   0x0B
#define HAP_TLV_FRAGMENT_DATA 0x0C
#define HAP_TLV_FRAGMENT_LAST 0x0D
#define HAP_TLV_SESSION_ID    0x0E
#define HAP_TLV_SEPARATOR     0xFF

/* HAP pairing method */
#define HAP_METHOD_TRANSIENT  0x10

typedef enum {
    PAIR_STATE_IDLE       = 0,
    PAIR_STATE_SETUP_M1   = 1,   /* waiting for M1 from iPhone */
    PAIR_STATE_SETUP_M2   = 2,   /* M2 sent, waiting for M3 */
    PAIR_STATE_SETUP_DONE = 3,   /* setup complete */
    PAIR_STATE_VERIFY_M1  = 4,   /* waiting for verify M1 */
    PAIR_STATE_VERIFY_M2  = 5,   /* verify M2 sent */
    PAIR_STATE_VERIFIED   = 6,   /* session fully authenticated */
    PAIR_STATE_ERROR      = -1,
} pair_state_t;

typedef struct {
    pair_state_t state;

    /* Server's long-term Ed25519 keypair (generated once at startup) */
    uint8_t ed25519_pub[32];
    uint8_t ed25519_priv[64];   /* OpenSSL "private" = seed || pub = 64 bytes */

    /* X25519 ephemeral keypair (regenerated each pair-verify) */
    uint8_t x25519_pub[32];
    uint8_t x25519_priv[32];

    /* SRP-6a state */
    uint8_t srp_salt[16];
    uint8_t srp_session_key[64];
    bool    srp_key_valid;

    /* Shared session key derived after pair-verify */
    uint8_t session_key[32];
    bool    session_key_valid;

    /* Server identifier (UUID string, NUL-terminated) */
    char    server_id[64];
} airplay_pair_ctx_t;

/*
 * Initialise pairing context.
 * Generates the server's long-term Ed25519 keypair.
 * server_id should be a UUID string (e.g. "AA:BB:CC:DD:EE:FF").
 */
int pair_ctx_init(airplay_pair_ctx_t *ctx, const char *server_id);

/*
 * Load a previously saved Ed25519 keypair (optional — allows persistent identity).
 * pub must be 32 bytes, priv must be 64 bytes.
 */
void pair_ctx_load_keypair(airplay_pair_ctx_t *ctx,
                            const uint8_t pub[32], const uint8_t priv[64]);

/*
 * Get the server's Ed25519 public key (32 bytes).
 */
void pair_ctx_get_ed25519_pub(const airplay_pair_ctx_t *ctx, uint8_t pub[32]);

/*
 * Process a /pair-setup POST body (binary TLV8 data).
 *
 * in_data / in_len  : raw TLV8 bytes from the POST body
 * out_data          : caller-supplied buffer (>= 1024 bytes)
 * out_len           : set to number of valid bytes in out_data
 *
 * Returns 0 on success, -1 on error.
 */
int pair_setup_process(airplay_pair_ctx_t *ctx,
                       const uint8_t *in_data, size_t in_len,
                       uint8_t *out_data, size_t *out_len);

/*
 * Process a /pair-verify POST body (binary TLV8 data).
 *
 * Returns 0 on success, -1 on error.
 * After success with state == PAIR_STATE_VERIFIED, the session key is available.
 */
int pair_verify_process(airplay_pair_ctx_t *ctx,
                         const uint8_t *in_data, size_t in_len,
                         uint8_t *out_data, size_t *out_len);

/*
 * Retrieve the 32-byte shared session key (available after PAIR_STATE_VERIFIED).
 * Returns 0 on success, -1 if not yet available.
 */
int pair_get_session_key(const airplay_pair_ctx_t *ctx, uint8_t key[32]);

pair_state_t pair_get_state(const airplay_pair_ctx_t *ctx);

void pair_ctx_destroy(airplay_pair_ctx_t *ctx);

/* -----------------------------------------------------------------------
 * TLV8 helpers (used internally and by rtsp layer)
 * ----------------------------------------------------------------------- */

/*
 * Encode one TLV8 item into buf.  Returns number of bytes written.
 * (Each TLV8 item is: type(1) + length(1..N) + value(length))
 * For values > 255 bytes, the encoder automatically fragments at 255.
 */
size_t tlv8_encode_item(uint8_t *buf, size_t buf_cap,
                         uint8_t type, const uint8_t *val, size_t val_len);

/*
 * Find a TLV8 item by type in `data`.
 * Returns pointer to value bytes; sets *val_len on success.
 * Returns NULL if not found.
 * NOTE: For values >255 bytes (multi-fragment), use tlv8_find_reassemble().
 */
const uint8_t *tlv8_find(const uint8_t *data, size_t data_len,
                          uint8_t type, size_t *val_len);

/*
 * Find and reassemble a multi-fragment TLV8 value.
 * Values >255 bytes are split across consecutive TLV items with the same type.
 * Returns a malloc'd buffer containing the full value; caller must free().
 * Sets *total_len to the total reassembled length.
 * Returns NULL if the type is not found.
 */
uint8_t *tlv8_find_reassemble(const uint8_t *data, size_t data_len,
                                uint8_t type, size_t *total_len);
