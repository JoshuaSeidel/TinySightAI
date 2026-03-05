#include "iap2_carplay.h"
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Append a TLV to buf at *pos.
 * Format: [2-byte total_len (incl. header)][2-byte tag][data]
 * Returns number of bytes written.
 */
static size_t tlv_append(uint8_t *buf, size_t buf_size, size_t *pos,
                          uint16_t tag, const void *val, uint16_t val_len)
{
    uint16_t total = (uint16_t)(4 + val_len);
    if (*pos + total > buf_size) {
        fprintf(stderr, "iap2: carplay — TLV buffer overflow\n");
        return 0;
    }
    uint8_t *p = buf + *pos;
    p[0] = (total >> 8) & 0xFF;
    p[1] =  total       & 0xFF;
    p[2] = (tag >> 8)   & 0xFF;
    p[3] =  tag         & 0xFF;
    if (val && val_len > 0)
        memcpy(p + 4, val, val_len);
    *pos += total;
    return total;
}

/*
 * Find a TLV by tag within data[0..len].
 * Sets *out_val and *out_len on match; returns 0 on success, -1 if not found.
 */
static int tlv_find(const uint8_t *data, size_t len, uint16_t tag,
                    const uint8_t **out_val, size_t *out_len)
{
    size_t pos = 0;
    while (pos + 4 <= len) {
        uint16_t tlv_total = (uint16_t)((data[pos] << 8) | data[pos + 1]);
        uint16_t tlv_tag   = (uint16_t)((data[pos + 2] << 8) | data[pos + 3]);
        if (tlv_total < 4 || pos + tlv_total > len)
            break; /* malformed */
        if (tlv_tag == tag) {
            *out_val = data + pos + 4;
            *out_len = tlv_total - 4;
            return 0;
        }
        pos += tlv_total;
    }
    return -1; /* not found */
}

/*
 * Send an iAP2 control-channel message.
 * msg_id: one of IAP2_MSG_* from iap2_session.h
 * params/params_len: TLV-encoded parameters (may be NULL)
 */
static int send_ctrl_msg(iap2_carplay_t *cp, uint16_t msg_id,
                          const uint8_t *params, size_t params_len)
{
    size_t msg_len = 4 + params_len;
    uint8_t buf[512];
    if (msg_len > sizeof(buf)) {
        fprintf(stderr, "iap2: carplay — control message too large\n");
        return -1;
    }

    buf[0] = (msg_len >> 8) & 0xFF;
    buf[1] =  msg_len       & 0xFF;
    buf[2] = (msg_id >> 8)  & 0xFF;
    buf[3] =  msg_id        & 0xFF;
    if (params && params_len > 0)
        memcpy(buf + 4, params, params_len);

    return iap2_link_send_data(cp->session->link,
                               IAP2_SESSION_CONTROL, buf, msg_len);
}

/*
 * Send a CarPlay sub-message on the EAP channel.
 * cp_msg_id: one of CP_MSG_* defined in iap2_carplay.h
 * payload/payload_len: TLV-encoded body
 */
static int send_eap_msg(iap2_carplay_t *cp, uint16_t cp_msg_id,
                         const uint8_t *payload, size_t payload_len)
{
    /* EAP payload: [2-byte cp_msg_id][TLV data...] */
    size_t total = 2 + payload_len;
    uint8_t buf[1024];
    if (total > sizeof(buf)) {
        fprintf(stderr, "iap2: carplay — EAP message too large\n");
        return -1;
    }
    buf[0] = (cp_msg_id >> 8) & 0xFF;
    buf[1] =  cp_msg_id       & 0xFF;
    if (payload && payload_len > 0)
        memcpy(buf + 2, payload, payload_len);

    return iap2_link_send_data(cp->session->link,
                               IAP2_SESSION_EAP, buf, total);
}

/* Build and send StartExternalAccessoryProtocolSession */
static int send_start_eap(iap2_carplay_t *cp)
{
    uint8_t params[128];
    size_t pos = 0;

    /* TLV 0x0000: ExternalAccessoryProtocolIdentifier (string) */
    const char *proto_id = "com.apple.carplay";
    tlv_append(params, sizeof(params), &pos,
               0x0000, proto_id, (uint16_t)strlen(proto_id));

    /* TLV 0x0001: ExternalAccessoryProtocolSessionIdentifier (uint8) */
    uint8_t sess_id = cp->eap_session_id;
    tlv_append(params, sizeof(params), &pos, 0x0001, &sess_id, 1);

    printf("iap2: carplay — sending StartEAPSession (proto=com.apple.carplay, sid=%d)\n",
           sess_id);
    return send_ctrl_msg(cp, IAP2_MSG_START_EAP_SESSION, params, pos);
}

/* Build and send StopExternalAccessoryProtocolSession */
static int send_stop_eap(iap2_carplay_t *cp)
{
    uint8_t params[16];
    size_t pos = 0;

    /* TLV 0x0001: session identifier */
    uint8_t sess_id = cp->eap_session_id;
    tlv_append(params, sizeof(params), &pos, 0x0001, &sess_id, 1);

    printf("iap2: carplay — sending StopEAPSession (sid=%d)\n", sess_id);
    return send_ctrl_msg(cp, IAP2_MSG_STOP_EAP_SESSION, params, pos);
}

/*
 * Send our WiFi credentials to the iPhone so it can connect to the
 * Radxa's hidden AP and reach the AirPlay server.
 */
static int send_wifi_credentials(iap2_carplay_t *cp)
{
    uint8_t tlv_buf[256];
    size_t pos = 0;

    tlv_append(tlv_buf, sizeof(tlv_buf), &pos,
               CP_TLV_SSID, CP_WIFI_SSID, (uint16_t)strlen(CP_WIFI_SSID));

    tlv_append(tlv_buf, sizeof(tlv_buf), &pos,
               CP_TLV_PASSPHRASE, CP_WIFI_PASSPHRASE,
               (uint16_t)strlen(CP_WIFI_PASSPHRASE));

    uint8_t sec = 0x02; /* WPA2 */
    tlv_append(tlv_buf, sizeof(tlv_buf), &pos, CP_TLV_SECURITY_MODE, &sec, 1);

    uint16_t port_be = htons(CP_AIRPLAY_PORT);
    tlv_append(tlv_buf, sizeof(tlv_buf), &pos,
               CP_TLV_AIRPLAY_PORT, &port_be, 2);

    printf("iap2: carplay — sending WiFi credentials (SSID=%s, AirPlay port=%d)\n",
           CP_WIFI_SSID, CP_AIRPLAY_PORT);
    return send_eap_msg(cp, CP_MSG_WIFI_CREDENTIALS, tlv_buf, pos);
}

/*
 * Parse WiFi credentials sent by the iPhone (it may provide its own
 * WiFi Direct network instead of connecting to ours).
 */
static void parse_wifi_credentials(iap2_carplay_t *cp,
                                    const uint8_t *data, size_t len)
{
    const uint8_t *val;
    size_t val_len;

    if (tlv_find(data, len, CP_TLV_SSID, &val, &val_len) == 0 && val_len > 0) {
        size_t copy_len = val_len < sizeof(cp->wifi.ssid) - 1
                          ? val_len : sizeof(cp->wifi.ssid) - 1;
        memcpy(cp->wifi.ssid, val, copy_len);
        cp->wifi.ssid[copy_len] = '\0';
    }

    if (tlv_find(data, len, CP_TLV_PASSPHRASE, &val, &val_len) == 0 && val_len > 0) {
        size_t copy_len = val_len < sizeof(cp->wifi.passphrase) - 1
                          ? val_len : sizeof(cp->wifi.passphrase) - 1;
        memcpy(cp->wifi.passphrase, val, copy_len);
        cp->wifi.passphrase[copy_len] = '\0';
    }

    if (tlv_find(data, len, CP_TLV_BSSID, &val, &val_len) == 0 && val_len == 6) {
        memcpy(cp->wifi.bssid, val, 6);
    }

    if (tlv_find(data, len, CP_TLV_SECURITY_MODE, &val, &val_len) == 0 && val_len == 1) {
        cp->wifi.security_mode = val[0];
    }

    if (tlv_find(data, len, CP_TLV_AIRPLAY_PORT, &val, &val_len) == 0 && val_len == 2) {
        uint16_t port_be;
        memcpy(&port_be, val, 2);
        cp->wifi.airplay_port = ntohs(port_be);
    } else {
        cp->wifi.airplay_port = CP_AIRPLAY_PORT;
    }

    printf("iap2: carplay — iPhone WiFi creds: SSID=%s port=%d\n",
           cp->wifi.ssid, cp->wifi.airplay_port);
}

/*
 * Handle the CarPlay StartWirelessCarPlaySession sub-message.
 * data/len are the TLV body after the 2-byte sub-message ID.
 */
static void handle_start_wireless_session(iap2_carplay_t *cp,
                                           const uint8_t *data, size_t len)
{
    printf("iap2: carplay — received StartWirelessCarPlaySession\n");

    /* Check if iPhone provided its own WiFi credentials */
    const uint8_t *val;
    size_t val_len;
    if (tlv_find(data, len, CP_TLV_SSID, &val, &val_len) == 0 && val_len > 0) {
        /* iPhone provided its WiFi Direct credentials — use them */
        parse_wifi_credentials(cp, data, len);
        cp->we_provide_wifi = false;
        printf("iap2: carplay — using iPhone WiFi Direct network\n");
    } else {
        /*
         * iPhone did not supply credentials — send ours so it can
         * connect to the Radxa AP.
         */
        cp->we_provide_wifi = true;
        strncpy(cp->wifi.ssid, CP_WIFI_SSID, sizeof(cp->wifi.ssid) - 1);
        strncpy(cp->wifi.passphrase, CP_WIFI_PASSPHRASE,
                sizeof(cp->wifi.passphrase) - 1);
        cp->wifi.security_mode = 0x02; /* WPA2 */
        cp->wifi.airplay_port  = CP_AIRPLAY_PORT;
        if (send_wifi_credentials(cp) < 0) {
            fprintf(stderr, "iap2: carplay — failed to send WiFi credentials\n");
            cp->state = IAP2_CP_ERROR;
            return;
        }
    }

    cp->state = IAP2_CP_SESSION_ACTIVE;
    printf("iap2: carplay — wireless CarPlay session active\n");

    /* Notify main daemon — AirPlay server can now expect the iPhone */
    if (cp->ready_cb)
        cp->ready_cb(&cp->wifi, cp->ready_ctx);
}

static void handle_stop_wireless_session(iap2_carplay_t *cp)
{
    printf("iap2: carplay — received StopWirelessCarPlaySession\n");
    cp->state = IAP2_CP_STOPPED;
    if (cp->stopped_cb)
        cp->stopped_cb(cp->stopped_ctx);
}

static void handle_wireless_update(iap2_carplay_t *cp,
                                    const uint8_t *data, size_t len)
{
    /* Status update from iPhone — log and ignore for now */
    printf("iap2: carplay — WirelessCarPlayUpdate (%zu bytes)\n", len);
    (void)cp; (void)data;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void iap2_carplay_init(iap2_carplay_t *cp,
                       iap2_cp_ready_cb_t ready_cb, void *ready_ctx,
                       iap2_cp_stopped_cb_t stopped_cb, void *stopped_ctx)
{
    memset(cp, 0, sizeof(*cp));
    cp->state      = IAP2_CP_IDLE;
    cp->eap_session_id = 1; /* arbitrary starting session ID */
    cp->ready_cb   = ready_cb;
    cp->ready_ctx  = ready_ctx;
    cp->stopped_cb = stopped_cb;
    cp->stopped_ctx = stopped_ctx;
    printf("iap2: carplay handler initialised\n");
}

int iap2_carplay_start(iap2_carplay_t *cp, iap2_session_t *session)
{
    if (cp->state != IAP2_CP_IDLE) {
        fprintf(stderr, "iap2: carplay_start in non-IDLE state (%d)\n",
                (int)cp->state);
        return -1;
    }

    cp->session = session;
    cp->state   = IAP2_CP_EAP_STARTING;

    if (send_start_eap(cp) < 0) {
        fprintf(stderr, "iap2: carplay — failed to send StartEAPSession\n");
        cp->state = IAP2_CP_ERROR;
        return -1;
    }
    return 0;
}

int iap2_carplay_handle_message(iap2_carplay_t *cp,
                                  uint16_t msg_id,
                                  const uint8_t *data, size_t len)
{
    /*
     * Control-channel messages we handle:
     *   IAP2_MSG_EAP_SESSION_STARTED (0xAA22) — EAP session confirmed
     *   IAP2_MSG_STOP_EAP_SESSION    (0xAA21) — iPhone stopping
     *
     * EAP-channel messages arrive with msg_id = 0xFFFF (sentinel) and
     * the EAP sub-message ID in the first 2 bytes of data.
     * The caller should pass EAP payloads as msg_id=0x0000 and provide
     * the raw EAP payload in data/len.
     *
     * For simplicity, we also handle the raw EAP payload inline here
     * when msg_id == IAP2_SESSION_EAP (0x02 used as a sentinel from
     * the dispatcher) — but the session dispatcher can pass 0x0000 for EAP.
     */

    switch (msg_id) {

    case IAP2_MSG_EAP_SESSION_STARTED:   /* 0xAA22 */
        if (cp->state != IAP2_CP_EAP_STARTING) {
            fprintf(stderr, "iap2: carplay — EAPSessionStarted in state %d\n",
                    (int)cp->state);
        }
        printf("iap2: carplay — EAP session started (CarPlay channel open)\n");
        cp->state = IAP2_CP_EAP_RUNNING;
        return 0;

    case IAP2_MSG_STOP_EAP_SESSION:      /* 0xAA21 */
        printf("iap2: carplay — iPhone sent StopEAPSession\n");
        cp->state = IAP2_CP_STOPPED;
        if (cp->stopped_cb)
            cp->stopped_cb(cp->stopped_ctx);
        return 0;

    case 0x0000:
    /*
     * EAP payload from the session dispatcher.
     * First 2 bytes are the CarPlay sub-message ID.
     */
        if (len < 2) {
            fprintf(stderr, "iap2: carplay — EAP payload too short\n");
            return 0;
        }
        {
            uint16_t cp_msg_id = (uint16_t)((data[0] << 8) | data[1]);
            const uint8_t *body = data + 2;
            size_t body_len = len - 2;

            switch (cp_msg_id) {
            case CP_MSG_START_WIRELESS_SESSION:
                handle_start_wireless_session(cp, body, body_len);
                break;
            case CP_MSG_STOP_WIRELESS_SESSION:
                handle_stop_wireless_session(cp);
                break;
            case CP_MSG_WIRELESS_UPDATE:
                handle_wireless_update(cp, body, body_len);
                break;
            default:
                printf("iap2: carplay — unknown EAP sub-message 0x%04X\n",
                       cp_msg_id);
                break;
            }
        }
        return 0;

    default:
        return -1; /* not a CarPlay message */
    }
}

void iap2_carplay_on_airplay_ready(iap2_carplay_t *cp)
{
    printf("iap2: carplay — AirPlay stream established, CarPlay fully active\n");
    if (cp->state != IAP2_CP_SESSION_ACTIVE) {
        fprintf(stderr,
                "iap2: carplay — airplay_ready called in unexpected state %d\n",
                (int)cp->state);
    }
    /* Nothing further to send over iAP2 — the AirPlay session carries video */
}

int iap2_carplay_stop(iap2_carplay_t *cp)
{
    if (cp->state == IAP2_CP_IDLE ||
        cp->state == IAP2_CP_STOPPED ||
        cp->state == IAP2_CP_STOPPING) {
        return 0;
    }

    cp->state = IAP2_CP_STOPPING;

    if (send_stop_eap(cp) < 0) {
        fprintf(stderr, "iap2: carplay — failed to send StopEAPSession\n");
        cp->state = IAP2_CP_ERROR;
        return -1;
    }

    cp->state = IAP2_CP_STOPPED;
    printf("iap2: carplay — session stopped\n");
    if (cp->stopped_cb)
        cp->stopped_cb(cp->stopped_ctx);

    return 0;
}
