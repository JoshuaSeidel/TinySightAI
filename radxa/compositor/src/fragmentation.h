/*
 * fragmentation.h — AAP frame fragmentation and reassembly
 *
 * Android Auto Protocol splits large messages into fragments when they
 * exceed AAP_MAX_FRAME_SIZE (16384 bytes). Each channel manages its own
 * independent fragment stream; the first fragment carries a 'total_len'
 * field in the AAP header that the reassembler uses.
 *
 * Flag byte semantics (from aa_protocol.h / observed traffic):
 *   0x08  single (no fragmentation)          AAP_FLAG_SINGLE
 *   0x09  first fragment                     AAP_FLAG_FIRST
 *   0x0A  middle fragment (continuation)     AAP_FLAG_CONT
 *   0x0B  last fragment                      AAP_FLAG_LAST
 *
 * Per-channel reassembly buffers are held inside frag_ctx_t. Each call
 * to frag_submit() either accumulates fragment data or completes a
 * message. On completion the caller receives a pointer to the assembled
 * buffer; the pointer remains valid until the next call to frag_submit()
 * on the same channel, or until frag_ctx_destroy().
 *
 * The fragmenter (frag_split) takes a complete message and a callback;
 * it calls the callback once for each fragment with proper flags set.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Maximum reassembled message we will accept (512 KB) */
#define FRAG_MAX_MESSAGE_SIZE   (512 * 1024)

/* Maximum number of independent channels tracked */
#define FRAG_MAX_CHANNELS       16

/* Fragment size when splitting (matches AAP_MAX_FRAME_SIZE) */
#define FRAG_SPLIT_SIZE         16384

/* Flag values (same as aa_protocol.h, reproduced here for self-containment) */
#define FRAG_FLAG_SINGLE  0x08
#define FRAG_FLAG_FIRST   0x09
#define FRAG_FLAG_CONT    0x0A
#define FRAG_FLAG_LAST    0x0B

/* Per-channel reassembly state */
typedef struct {
    uint8_t *buf;           /* allocated reassembly buffer */
    size_t   buf_size;      /* allocated capacity */
    size_t   total_len;     /* expected total from first-fragment header */
    size_t   received;      /* bytes accumulated so far */
    int      active;        /* 1 if we are mid-reassembly */
} frag_channel_t;

/* Context holding state for all channels */
typedef struct {
    frag_channel_t channels[FRAG_MAX_CHANNELS];
} frag_ctx_t;

/**
 * Callback type for frag_split().
 * Called once per output fragment.
 * @param channel    Channel ID (passed through from frag_split)
 * @param flags      Fragment flag byte (FRAG_FLAG_*)
 * @param data       Fragment payload (slice of original data)
 * @param len        Payload length
 * @param user_ctx   Caller-supplied context pointer
 */
typedef void (*frag_split_cb)(uint8_t channel, uint8_t flags,
                               const uint8_t *data, size_t len,
                               void *user_ctx);

/**
 * Initialise a reassembly context. Must be called before any other
 * frag_* function on the same context.
 */
void frag_ctx_init(frag_ctx_t *ctx);

/**
 * Submit a fragment for channel `channel`.
 *
 * @param ctx          Reassembly context
 * @param channel      Channel ID (0-15)
 * @param flags        Fragment flags from AAP header byte 1
 * @param data         Fragment payload (after the 6-byte AAP header)
 * @param len          Payload length
 * @param out_complete On success, set to a pointer to the assembled
 *                     message (owned by ctx, valid until next call on
 *                     this channel).  Set to NULL if the message is not
 *                     yet complete.
 * @param out_len      Set to assembled message length when complete.
 *
 * @return  0   if processing was successful (check *out_complete)
 *         -1   on error (out-of-order start, oversized, allocation failure)
 */
int frag_submit(frag_ctx_t *ctx,
                uint8_t channel, uint8_t flags,
                const uint8_t *data, size_t len,
                uint8_t **out_complete, size_t *out_len);

/**
 * Fragment a complete message and deliver each piece via callback.
 *
 * @param channel   Channel ID written into callback
 * @param data      Complete message bytes
 * @param len       Message length
 * @param total_len Full message length for the first-fragment header
 *                  (pass same value as len for a single complete message)
 * @param callback  Called once per fragment
 * @param user_ctx  Passed through to callback unchanged
 */
void frag_split(uint8_t channel,
                const uint8_t *data, size_t len,
                frag_split_cb callback, void *user_ctx);

/**
 * Free all reassembly buffers in ctx. The context itself is not freed
 * (it is typically stack- or struct-allocated).
 */
void frag_ctx_destroy(frag_ctx_t *ctx);
