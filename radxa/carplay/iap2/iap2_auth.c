#include "iap2_auth.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Current monotonic time in seconds */
static time_t now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void set_state(iap2_auth_t *auth, iap2_auth_state_t new_state)
{
    auth->state = new_state;
    auth->last_msg_time = now_sec();
    printf("iap2: auth state → %d\n", (int)new_state);
}

/*
 * Build and send an iAP2 session message carrying a single TLV parameter.
 * The message is sent on IAP2_SESSION_CONTROL.
 *
 * tag 0x0000 is the conventional "payload" TLV for auth messages.
 */
static int send_auth_message(iap2_auth_t *auth, uint16_t msg_id,
                              uint16_t tlv_tag,
                              const uint8_t *data, size_t data_len)
{
    /* TLV: [2-byte total_len][2-byte tag][data] */
    uint16_t tlv_total = (uint16_t)(4 + data_len);
    /* iAP2 session message: [2-byte msg_len][2-byte msg_id][tlv...] */
    size_t msg_len = 4 + tlv_total;

    uint8_t buf[1600];
    if (msg_len > sizeof(buf)) {
        fprintf(stderr, "iap2: auth message too large (%zu bytes)\n", msg_len);
        return -1;
    }

    /* Message header */
    buf[0] = (msg_len >> 8) & 0xFF;
    buf[1] =  msg_len       & 0xFF;
    buf[2] = (msg_id >> 8)  & 0xFF;
    buf[3] =  msg_id        & 0xFF;

    /* TLV */
    buf[4] = (tlv_total >> 8) & 0xFF;
    buf[5] =  tlv_total       & 0xFF;
    buf[6] = (tlv_tag >> 8)   & 0xFF;
    buf[7] =  tlv_tag         & 0xFF;
    if (data && data_len > 0)
        memcpy(buf + 8, data, data_len);

    return iap2_link_send_data(auth->session->link,
                               IAP2_SESSION_CONTROL, buf, msg_len);
}

/* Send the MFi certificate to the iPhone */
static int send_cert(iap2_auth_t *auth)
{
    uint8_t cert[1024];
    int cert_len = mfi_get_certificate(&auth->mfi, cert, sizeof(cert));
    if (cert_len <= 0) {
        fprintf(stderr, "iap2: auth — cannot read MFi certificate\n");
        return -1;
    }

    printf("iap2: auth — sending certificate (%d bytes)\n", cert_len);
    return send_auth_message(auth, IAP2_AUTH_MSG_CERT_RESPONSE,
                             0x0000, cert, (size_t)cert_len);
}

/* Sign challenge and send response */
static int send_challenge_response(iap2_auth_t *auth,
                                   const uint8_t *challenge, size_t chal_len)
{
    uint8_t sig[256];
    int sig_len = mfi_sign_challenge(&auth->mfi,
                                     challenge, chal_len,
                                     sig, sizeof(sig));
    if (sig_len <= 0) {
        fprintf(stderr, "iap2: auth — MFi signing failed\n");
        return -1;
    }

    printf("iap2: auth — sending challenge response (%d bytes)\n", sig_len);
    return send_auth_message(auth, IAP2_AUTH_MSG_CHALLENGE_RESPONSE,
                             0x0000, sig, (size_t)sig_len);
}

/*
 * Extract TLV payload from an incoming auth message.
 * Assumes first TLV at offset 0 contains the relevant blob.
 * Sets *out_data and *out_len; returns 0 on success.
 */
static int extract_tlv_payload(const uint8_t *data, size_t len,
                                const uint8_t **out_data, size_t *out_len)
{
    if (len < 4) {
        fprintf(stderr, "iap2: auth — TLV too short (%zu bytes)\n", len);
        return -1;
    }
    uint16_t tlv_total = (uint16_t)((data[0] << 8) | data[1]);
    /* tlv_tag = (data[2] << 8) | data[3]; — not validated here */
    if (tlv_total < 4 || (size_t)tlv_total > len) {
        fprintf(stderr, "iap2: auth — TLV length invalid (%u)\n", tlv_total);
        return -1;
    }
    *out_data = data + 4;
    *out_len  = tlv_total - 4;
    return 0;
}

/* Permanently fail auth and invoke done callback */
static void auth_fail(iap2_auth_t *auth, const char *reason)
{
    fprintf(stderr, "iap2: auth FAILED — %s\n", reason);
    set_state(auth, IAP2_AUTH_ERROR);
    if (auth->done_cb)
        auth->done_cb(false, auth->done_ctx);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int iap2_auth_init(iap2_auth_t *auth,
                   const char *i2c_device, int chip_addr,
                   iap2_auth_done_cb_t done_cb, void *done_ctx)
{
    memset(auth, 0, sizeof(*auth));
    auth->state       = IAP2_AUTH_IDLE;
    auth->done_cb     = done_cb;
    auth->done_ctx    = done_ctx;
    auth->retry_count = 0;

    if (pthread_mutex_init(&auth->mutex, NULL) != 0) {
        fprintf(stderr, "iap2: auth — pthread_mutex_init failed: %s\n",
                strerror(errno));
        return -1;
    }

    if (mfi_open(&auth->mfi, i2c_device, (uint8_t)chip_addr) < 0) {
        fprintf(stderr, "iap2: auth — cannot open MFi chip at %s addr=0x%02X\n",
                i2c_device, chip_addr);
        pthread_mutex_destroy(&auth->mutex);
        return -1;
    }

    int ver = mfi_get_version(&auth->mfi);
    if (ver < 0) {
        fprintf(stderr, "iap2: auth — WARNING: MFi chip version read failed\n");
        /* Non-fatal: chip may still work for signing */
    } else {
        printf("iap2: auth — MFi chip version 0x%02X\n", ver);
    }

    printf("iap2: auth initialised (I2C %s, addr 0x%02X)\n",
           i2c_device, chip_addr);
    return 0;
}

int iap2_auth_start(iap2_auth_t *auth, iap2_session_t *session)
{
    pthread_mutex_lock(&auth->mutex);

    if (auth->state != IAP2_AUTH_IDLE) {
        fprintf(stderr, "iap2: auth_start called in non-IDLE state (%d)\n",
                (int)auth->state);
        pthread_mutex_unlock(&auth->mutex);
        return -1;
    }

    auth->session = session;
    auth->retry_count = 0;
    set_state(auth, IAP2_AUTH_CERT_REQUESTED);

    printf("iap2: auth started — waiting for RequestAuthenticationCertificate\n");

    pthread_mutex_unlock(&auth->mutex);
    return 0;
}

int iap2_auth_handle_message(iap2_auth_t *auth,
                              uint16_t msg_id,
                              const uint8_t *data, size_t len)
{
    /* Quick check before locking: is this an auth message at all? */
    if (msg_id < IAP2_AUTH_MSG_CERT_REQUEST || msg_id > IAP2_AUTH_MSG_FAILED)
        return -1; /* not an auth message */

    pthread_mutex_lock(&auth->mutex);

    int ret = 0;

    switch (msg_id) {

    /* ------------------------------------------------------------------ */
    case IAP2_AUTH_MSG_CERT_REQUEST:   /* 0xAA00 */
    /*
     * iPhone asks for our MFi certificate.  Valid in CERT_REQUESTED state.
     * Also accepted if we are in CERT_SENT (re-request after retry).
     */
        if (auth->state != IAP2_AUTH_CERT_REQUESTED &&
            auth->state != IAP2_AUTH_CERT_SENT) {
            fprintf(stderr, "iap2: auth — unexpected CERT_REQUEST in state %d\n",
                    (int)auth->state);
            break;
        }
        printf("iap2: auth — iPhone requests certificate\n");
        if (send_cert(auth) < 0) {
            auth_fail(auth, "failed to send certificate");
            ret = 0; /* handled, even though failed */
            break;
        }
        auth->retry_count = 0;
        set_state(auth, IAP2_AUTH_CERT_SENT);
        break;

    /* ------------------------------------------------------------------ */
    case IAP2_AUTH_MSG_CHALLENGE_REQUEST:  /* 0xAA02 */
    /*
     * iPhone sends us a challenge to sign.
     * Valid after we have sent the certificate.
     */
        if (auth->state != IAP2_AUTH_CERT_SENT &&
            auth->state != IAP2_AUTH_CHALLENGE_RECEIVED) {
            fprintf(stderr, "iap2: auth — unexpected CHALLENGE_REQUEST in state %d\n",
                    (int)auth->state);
            break;
        }
        set_state(auth, IAP2_AUTH_CHALLENGE_RECEIVED);

        const uint8_t *challenge = NULL;
        size_t chal_len = 0;
        if (extract_tlv_payload(data, len, &challenge, &chal_len) < 0) {
            auth_fail(auth, "malformed challenge TLV");
            break;
        }
        if (chal_len == 0 || chal_len > 256) {
            fprintf(stderr, "iap2: auth — invalid challenge length (%zu)\n", chal_len);
            auth_fail(auth, "invalid challenge length");
            break;
        }

        printf("iap2: auth — signing challenge (%zu bytes)\n", chal_len);
        if (send_challenge_response(auth, challenge, chal_len) < 0) {
            if (auth->retry_count < IAP2_AUTH_MAX_RETRIES) {
                auth->retry_count++;
                fprintf(stderr, "iap2: auth — signing failed, retry %d/%d\n",
                        auth->retry_count, IAP2_AUTH_MAX_RETRIES);
                /* Stay in CHALLENGE_RECEIVED — tick() will retry */
                auth->last_msg_time = now_sec(); /* reset timeout */
            } else {
                auth_fail(auth, "MFi signing failed after retries");
            }
            break;
        }
        auth->retry_count = 0;
        set_state(auth, IAP2_AUTH_CHALLENGE_SIGNED);
        break;

    /* ------------------------------------------------------------------ */
    case IAP2_AUTH_MSG_SUCCEEDED:   /* 0xAA04 */
        if (auth->state != IAP2_AUTH_CHALLENGE_SIGNED) {
            fprintf(stderr,
                    "iap2: auth — AuthSucceeded received in unexpected state %d\n",
                    (int)auth->state);
        }
        set_state(auth, IAP2_AUTH_COMPLETE);
        printf("iap2: auth — AuthenticationSucceeded!\n");
        if (auth->done_cb)
            auth->done_cb(true, auth->done_ctx);
        break;

    /* ------------------------------------------------------------------ */
    case IAP2_AUTH_MSG_FAILED:      /* 0xAA05 */
        fprintf(stderr, "iap2: auth — iPhone sent AuthenticationFailed\n");
        if (auth->retry_count < IAP2_AUTH_MAX_RETRIES) {
            auth->retry_count++;
            fprintf(stderr, "iap2: auth — retrying auth (attempt %d/%d)\n",
                    auth->retry_count, IAP2_AUTH_MAX_RETRIES);
            /* Reset to cert-request phase and wait */
            set_state(auth, IAP2_AUTH_CERT_REQUESTED);
        } else {
            auth_fail(auth, "AuthenticationFailed from iPhone, retries exhausted");
        }
        break;

    /* ------------------------------------------------------------------ */
    default:
        /* Should not reach here given the range check above */
        ret = -1;
        break;
    }

    pthread_mutex_unlock(&auth->mutex);
    return ret;
}

void iap2_auth_tick(iap2_auth_t *auth)
{
    pthread_mutex_lock(&auth->mutex);

    /* Nothing to do in terminal states */
    if (auth->state == IAP2_AUTH_IDLE ||
        auth->state == IAP2_AUTH_COMPLETE ||
        auth->state == IAP2_AUTH_ERROR) {
        pthread_mutex_unlock(&auth->mutex);
        return;
    }

    time_t elapsed = now_sec() - auth->last_msg_time;
    if (elapsed < IAP2_AUTH_TIMEOUT_SEC) {
        pthread_mutex_unlock(&auth->mutex);
        return;
    }

    /* Timed out waiting for iPhone */
    fprintf(stderr, "iap2: auth — timeout in state %d (elapsed %lds)\n",
            (int)auth->state, (long)elapsed);

    if (auth->retry_count < IAP2_AUTH_MAX_RETRIES) {
        auth->retry_count++;
        fprintf(stderr, "iap2: auth — timeout retry %d/%d\n",
                auth->retry_count, IAP2_AUTH_MAX_RETRIES);

        /*
         * Depending on current state, re-send the most recent message
         * to nudge the iPhone.
         */
        if (auth->state == IAP2_AUTH_CERT_SENT) {
            /* Re-send certificate */
            if (send_cert(auth) == 0)
                auth->last_msg_time = now_sec();
        }
        /* For CHALLENGE_SIGNED: we wait for iPhone; nothing to re-send. */
        auth->last_msg_time = now_sec();
    } else {
        auth_fail(auth, "timeout waiting for iPhone, retries exhausted");
    }

    pthread_mutex_unlock(&auth->mutex);
}

bool iap2_auth_is_complete(const iap2_auth_t *auth)
{
    return auth->state == IAP2_AUTH_COMPLETE;
}

bool iap2_auth_is_error(const iap2_auth_t *auth)
{
    return auth->state == IAP2_AUTH_ERROR;
}

void iap2_auth_cleanup(iap2_auth_t *auth)
{
    mfi_close(&auth->mfi);
    pthread_mutex_destroy(&auth->mutex);
    printf("iap2: auth cleaned up\n");
}
