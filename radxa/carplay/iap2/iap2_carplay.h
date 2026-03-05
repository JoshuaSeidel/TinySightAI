#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "iap2_session.h"

/*
 * iAP2 CarPlay Session Handler
 *
 * After iAP2 identification and MFi authentication succeed, this module
 * negotiates the CarPlay ExternalAccessoryProtocol (EAP) session and
 * exchanges wireless CarPlay credentials with the iPhone.
 *
 * Flow:
 *   1. iap2_carplay_start()
 *        → sends StartExternalAccessoryProtocolSession (0xAA20)
 *           with protocol identifier "com.apple.carplay"
 *
 *   2. iPhone replies with EAPSessionStarted (0xAA22)
 *        → we are now in the CarPlay EAP channel
 *
 *   3. iPhone sends StartWirelessCarPlaySession
 *        → contains its WiFi Direct credentials or requests ours
 *        → we extract/provide credentials and open AirPlay port
 *
 *   4. iap2_carplay_on_airplay_ready()
 *        → called by AirPlay server once mirroring begins
 *
 *   5. iap2_carplay_stop()
 *        → sends StopExternalAccessoryProtocolSession (0xAA21)
 *
 * Message IDs for CarPlay EAP channel (TLV-encoded within EAP payload):
 *   These are not standard iAP2 control IDs — they ride inside the
 *   EAP payload as CarPlay-specific control messages.
 *
 * CarPlay EAP sub-message IDs (big-endian uint16_t in first 2 bytes of payload):
 */

/* CarPlay sub-message IDs (inside EAP payload) */
#define CP_MSG_START_WIRELESS_SESSION  0x0001
#define CP_MSG_STOP_WIRELESS_SESSION   0x0002
#define CP_MSG_WIRELESS_UPDATE         0x0003
#define CP_MSG_WIFI_CREDENTIALS        0x0010
#define CP_MSG_AIRPLAY_CONFIG          0x0011

/* TLV parameter tags within CarPlay messages */
#define CP_TLV_SSID               0x0001
#define CP_TLV_PASSPHRASE         0x0002
#define CP_TLV_BSSID              0x0003
#define CP_TLV_SECURITY_MODE      0x0004  /* 0x02 = WPA2 */
#define CP_TLV_AIRPLAY_PORT       0x0005  /* uint16_t */
#define CP_TLV_SESSION_ID         0x0006  /* uint8_t */

/* WiFi configuration for the AirPlay channel — must match hostapd.conf */
#define CP_WIFI_SSID        "AADongle"
#define CP_WIFI_PASSPHRASE  "AADongle5GHz!"
#define CP_AIRPLAY_PORT     7000

typedef enum {
    IAP2_CP_IDLE = 0,
    IAP2_CP_EAP_STARTING,   /* StartEAP sent, waiting for EAPSessionStarted */
    IAP2_CP_EAP_RUNNING,    /* EAP session active */
    IAP2_CP_SESSION_ACTIVE, /* Wireless CarPlay session running */
    IAP2_CP_STOPPING,       /* StopEAP sent */
    IAP2_CP_STOPPED,
    IAP2_CP_ERROR,
} iap2_cp_state_t;

/* Credentials extracted or provided during session negotiation */
typedef struct {
    char    ssid[64];
    char    passphrase[64];
    uint8_t bssid[6];
    uint8_t security_mode;
    uint16_t airplay_port;
    uint8_t  session_id;
} cp_wifi_info_t;

/* Callback: CarPlay session is fully up, AirPlay may connect */
typedef void (*iap2_cp_ready_cb_t)(const cp_wifi_info_t *wifi, void *ctx);

/* Callback: CarPlay session has stopped */
typedef void (*iap2_cp_stopped_cb_t)(void *ctx);

typedef struct {
    iap2_cp_state_t state;
    iap2_session_t *session;
    uint8_t         eap_session_id;  /* assigned EAP session identifier */

    cp_wifi_info_t  wifi;            /* negotiated WiFi/AirPlay info */
    bool            we_provide_wifi; /* true → we sent our creds to iPhone */

    iap2_cp_ready_cb_t   ready_cb;
    void                *ready_ctx;
    iap2_cp_stopped_cb_t stopped_cb;
    void                *stopped_ctx;
} iap2_carplay_t;

/**
 * Initialise the CarPlay session handler.
 *
 * ready_cb   — invoked when the wireless CarPlay session is established.
 * stopped_cb — invoked when the session ends.
 */
void iap2_carplay_init(iap2_carplay_t *cp,
                       iap2_cp_ready_cb_t ready_cb, void *ready_ctx,
                       iap2_cp_stopped_cb_t stopped_cb, void *stopped_ctx);

/**
 * Begin CarPlay negotiation on an authenticated iAP2 session.
 * Sends StartExternalAccessoryProtocolSession to the iPhone.
 * Returns 0 on success, -1 on failure.
 */
int iap2_carplay_start(iap2_carplay_t *cp, iap2_session_t *session);

/**
 * Handle an incoming iAP2 message on the control or EAP channel.
 *
 * Returns 0 if the message was handled,
 *        -1 if it is not a CarPlay message (caller should handle it).
 */
int iap2_carplay_handle_message(iap2_carplay_t *cp,
                                 uint16_t msg_id,
                                 const uint8_t *data, size_t len);

/**
 * Called by the AirPlay server once the iPhone has connected and the
 * mirroring stream is active.  Logs the event and updates internal state.
 */
void iap2_carplay_on_airplay_ready(iap2_carplay_t *cp);

/**
 * Gracefully stop the CarPlay session.
 * Sends StopExternalAccessoryProtocolSession.
 */
int iap2_carplay_stop(iap2_carplay_t *cp);
