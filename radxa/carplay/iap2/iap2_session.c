#include "iap2_session.h"
#include "../mfi/mfi_auth.h"
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

/* External MFi device (initialized elsewhere, shared) */
extern mfi_device_t g_mfi_dev;

static int send_message(iap2_session_t *sess, uint8_t session_id,
                         uint16_t msg_id,
                         const uint8_t *params, size_t params_len)
{
    size_t msg_len = 4 + params_len; /* 2 (length) + 2 (id) + params */
    uint8_t buf[2048];
    if (msg_len > sizeof(buf)) return -1;

    buf[0] = (msg_len >> 8) & 0xFF;
    buf[1] = msg_len & 0xFF;
    buf[2] = (msg_id >> 8) & 0xFF;
    buf[3] = msg_id & 0xFF;
    if (params && params_len > 0)
        memcpy(buf + 4, params, params_len);

    return iap2_link_send_data(sess->link, session_id, buf, msg_len);
}

/* TLV helper: append a TLV to buffer */
static size_t tlv_append(uint8_t *buf, uint16_t tag, const void *val, uint16_t val_len)
{
    uint16_t total = 4 + val_len; /* 2 (length) + 2 (tag) + value */
    buf[0] = (total >> 8) & 0xFF;
    buf[1] = total & 0xFF;
    buf[2] = (tag >> 8) & 0xFF;
    buf[3] = tag & 0xFF;
    if (val && val_len > 0)
        memcpy(buf + 4, val, val_len);
    return total;
}

int iap2_session_init(iap2_session_t *sess, iap2_link_t *link)
{
    memset(sess, 0, sizeof(*sess));
    sess->link = link;

    /* Pre-load MFi certificate */
    sess->mfi_cert_len = mfi_get_certificate(&g_mfi_dev,
                                               sess->mfi_cert, sizeof(sess->mfi_cert));
    if (sess->mfi_cert_len < 0) {
        fprintf(stderr, "iap2: WARNING — MFi certificate not available\n");
        sess->mfi_cert_len = 0;
    }

    return 0;
}

int iap2_session_send_identification(iap2_session_t *sess)
{
    /*
     * IdentificationInformation message:
     * Contains TLV-encoded device info and supported features.
     */
    uint8_t params[512];
    size_t pos = 0;

    /* Device name */
    const char *name = "AADongle";
    pos += tlv_append(params + pos, 0x0000, name, strlen(name));

    /* Model identifier */
    const char *model = "AADongle-v1";
    pos += tlv_append(params + pos, 0x0001, model, strlen(model));

    /* Manufacturer */
    const char *mfg = "Custom";
    pos += tlv_append(params + pos, 0x0002, mfg, strlen(mfg));

    /* Serial number */
    const char *serial = "AADONGLE001";
    pos += tlv_append(params + pos, 0x0003, serial, strlen(serial));

    /* Firmware version */
    const char *fw = "1.0.0";
    pos += tlv_append(params + pos, 0x0004, fw, strlen(fw));

    /* Hardware version */
    const char *hw = "1.0";
    pos += tlv_append(params + pos, 0x0005, hw, strlen(hw));

    /* CarPlay supported — declare ExternalAccessoryProtocol */
    uint8_t cp_flag = 1;
    pos += tlv_append(params + pos, 0x0030, &cp_flag, 1);

    printf("iap2: sending IdentificationInformation (%zu bytes)\n", pos);
    return send_message(sess, IAP2_SESSION_CONTROL,
                         IAP2_MSG_IDENTIFICATION_INFO, params, pos);
}

int iap2_session_send_auth_cert(iap2_session_t *sess)
{
    if (sess->mfi_cert_len <= 0) {
        fprintf(stderr, "iap2: no MFi certificate available\n");
        return -1;
    }

    uint8_t params[1200];
    size_t pos = 0;
    pos += tlv_append(params + pos, 0x0000,
                       sess->mfi_cert, sess->mfi_cert_len);

    printf("iap2: sending AuthenticationCertificate (%d bytes)\n", sess->mfi_cert_len);
    return send_message(sess, IAP2_SESSION_CONTROL,
                         IAP2_MSG_AUTH_CERT_RESPONSE, params, pos);
}

int iap2_session_send_auth_response(iap2_session_t *sess,
                                     const uint8_t *challenge, size_t challenge_len)
{
    /* Sign challenge with MFi chip */
    uint8_t signature[256];
    int sig_len = mfi_sign_challenge(&g_mfi_dev,
                                      challenge, challenge_len,
                                      signature, sizeof(signature));
    if (sig_len < 0) {
        fprintf(stderr, "iap2: MFi signing failed\n");
        return -1;
    }

    uint8_t params[512];
    size_t pos = 0;
    pos += tlv_append(params + pos, 0x0000, signature, sig_len);

    printf("iap2: sending AuthenticationResponse (sig=%d bytes)\n", sig_len);
    return send_message(sess, IAP2_SESSION_CONTROL,
                         IAP2_MSG_AUTH_CHALLENGE_RESPONSE, params, pos);
}

int iap2_session_handle_message(iap2_session_t *sess,
                                 uint16_t msg_id,
                                 const uint8_t *payload, size_t payload_len)
{
    (void)payload_len;

    switch (msg_id) {
    case IAP2_MSG_IDENTIFICATION_ACCEPTED:
        printf("iap2: identification accepted by iPhone\n");
        sess->identified = true;
        break;

    case IAP2_MSG_IDENTIFICATION_REJECTED:
        fprintf(stderr, "iap2: identification REJECTED by iPhone\n");
        sess->identified = false;
        break;

    case IAP2_MSG_AUTH_CERT_REQUEST:
        printf("iap2: iPhone requesting authentication certificate\n");
        iap2_session_send_auth_cert(sess);
        break;

    case IAP2_MSG_AUTH_CHALLENGE_REQUEST:
        printf("iap2: iPhone sent authentication challenge\n");
        /* Extract challenge data from TLV */
        if (payload_len >= 4) {
            uint16_t tlv_len = (payload[0] << 8) | payload[1];
            /* uint16_t tlv_tag = (payload[2] << 8) | payload[3]; */
            if (tlv_len < 4 || (size_t)tlv_len > payload_len) {
                fprintf(stderr, "iap2: invalid challenge TLV length %u\n", tlv_len);
                break;
            }
            const uint8_t *challenge = payload + 4;
            size_t chal_len = tlv_len - 4;
            iap2_session_send_auth_response(sess, challenge, chal_len);
        }
        break;

    case IAP2_MSG_START_EAP_SESSION:
        printf("iap2: CarPlay EAP session starting\n");
        sess->authenticated = true;
        sess->carplay_started = true;
        /* Send EAP session started confirmation */
        send_message(sess, IAP2_SESSION_CONTROL,
                     IAP2_MSG_EAP_SESSION_STARTED, NULL, 0);
        break;

    default:
        printf("iap2: unhandled message 0x%04X (%zu bytes)\n",
               msg_id, payload_len);
        break;
    }

    return 0;
}
