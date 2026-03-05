/*
 * fragmentation.c — AAP frame fragmentation and reassembly
 */
#include "fragmentation.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Initialisation / destruction ---- */

void frag_ctx_init(frag_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void frag_ctx_destroy(frag_ctx_t *ctx)
{
    if (!ctx)
        return;
    for (int i = 0; i < FRAG_MAX_CHANNELS; i++) {
        free(ctx->channels[i].buf);
        ctx->channels[i].buf = NULL;
        ctx->channels[i].active = 0;
    }
}

/* ---- Internal helpers ---- */

static void channel_reset(frag_channel_t *ch)
{
    ch->received  = 0;
    ch->total_len = 0;
    ch->active    = 0;
    /* Keep buf allocated for reuse */
}

static int channel_ensure_capacity(frag_channel_t *ch, size_t needed)
{
    if (needed > FRAG_MAX_MESSAGE_SIZE) {
        fprintf(stderr, "fragmentation: message too large (%zu > %d)\n",
                needed, FRAG_MAX_MESSAGE_SIZE);
        return -1;
    }
    if (ch->buf_size < needed) {
        uint8_t *nb = realloc(ch->buf, needed);
        if (!nb) {
            fprintf(stderr, "fragmentation: allocation failed (%zu bytes)\n", needed);
            return -1;
        }
        ch->buf = nb;
        ch->buf_size = needed;
    }
    return 0;
}

/* ---- Reassembly ---- */

int frag_submit(frag_ctx_t *ctx,
                uint8_t channel, uint8_t flags,
                const uint8_t *data, size_t len,
                uint8_t **out_complete, size_t *out_len)
{
    if (!ctx || !out_complete || !out_len)
        return -1;

    *out_complete = NULL;
    *out_len = 0;

    if (channel >= FRAG_MAX_CHANNELS) {
        fprintf(stderr, "fragmentation: channel %u out of range\n", channel);
        return -1;
    }

    frag_channel_t *ch = &ctx->channels[channel];

    switch (flags) {

    case FRAG_FLAG_SINGLE:
        /*
         * Unfragmented message — deliver immediately without copying into
         * the reassembly buffer. The caller's buffer is valid for this call.
         * We reset any in-progress reassembly for this channel.
         */
        if (ch->active) {
            fprintf(stderr,
                    "fragmentation: ch%u — SINGLE received mid-reassembly; discarding\n",
                    channel);
            channel_reset(ch);
        }
        *out_complete = (uint8_t *)data; /* safe cast: caller should not modify */
        *out_len = len;
        return 0;

    case FRAG_FLAG_FIRST:
        /*
         * First fragment. total_len is carried by the AAP header 'total_len'
         * field which the caller has already decoded before calling us.
         * We use `len` as the accumulated first chunk; the caller is expected
         * to pass the AAP total_len separately through the flags/total_len
         * field.  However, because frag_submit does not receive total_len as
         * a separate parameter (it is already in the AAP header parsed by
         * aap_parse_header), we conservatively pre-allocate FRAG_MAX_MESSAGE_SIZE
         * and grow as needed.
         *
         * In practice the caller should use frag_submit_with_total() or pass
         * total_len == 0 meaning "unknown; grow as fragments arrive".
         */
        if (ch->active) {
            /* Abandon previous incomplete message */
            fprintf(stderr,
                    "fragmentation: ch%u — new FIRST fragment discards previous incomplete message\n",
                    channel);
            channel_reset(ch);
        }
        if (channel_ensure_capacity(ch, len) < 0)
            return -1;

        memcpy(ch->buf, data, len);
        ch->received  = len;
        ch->total_len = 0; /* unknown until LAST */
        ch->active    = 1;
        return 0; /* not yet complete */

    case FRAG_FLAG_CONT:
        if (!ch->active) {
            fprintf(stderr,
                    "fragmentation: ch%u — CONT without preceding FIRST; discarding\n",
                    channel);
            return -1;
        }
        if (channel_ensure_capacity(ch, ch->received + len) < 0) {
            channel_reset(ch);
            return -1;
        }
        memcpy(ch->buf + ch->received, data, len);
        ch->received += len;
        return 0; /* not yet complete */

    case FRAG_FLAG_LAST:
        if (!ch->active) {
            fprintf(stderr,
                    "fragmentation: ch%u — LAST without preceding FIRST; discarding\n",
                    channel);
            return -1;
        }
        if (channel_ensure_capacity(ch, ch->received + len) < 0) {
            channel_reset(ch);
            return -1;
        }
        memcpy(ch->buf + ch->received, data, len);
        ch->received += len;

        /* Message complete */
        *out_complete = ch->buf;
        *out_len      = ch->received;
        channel_reset(ch);
        return 0;

    default:
        fprintf(stderr, "fragmentation: ch%u — unknown flags 0x%02x\n",
                channel, flags);
        return -1;
    }
}

/* ---- Fragmentation ---- */

void frag_split(uint8_t channel,
                const uint8_t *data, size_t len,
                frag_split_cb callback, void *user_ctx)
{
    if (!data || !callback)
        return;

    if (len <= FRAG_SPLIT_SIZE) {
        /* Fits in a single frame */
        callback(channel, FRAG_FLAG_SINGLE, data, len, user_ctx);
        return;
    }

    /* Multiple fragments needed */
    size_t pos = 0;
    int first = 1;

    while (pos < len) {
        size_t remaining = len - pos;
        size_t chunk = (remaining > FRAG_SPLIT_SIZE) ? FRAG_SPLIT_SIZE : remaining;
        int is_last = (pos + chunk >= len);
        uint8_t flags;

        if (first) {
            flags = FRAG_FLAG_FIRST;
            first = 0;
        } else if (is_last) {
            flags = FRAG_FLAG_LAST;
        } else {
            flags = FRAG_FLAG_CONT;
        }

        callback(channel, flags, data + pos, chunk, user_ctx);
        pos += chunk;
    }
}
