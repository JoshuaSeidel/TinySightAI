#pragma once

#include "iap2_link.h"

/*
 * iAP2 Session Layer
 *
 * Handles session management and iAP2 control/data messages:
 *   - Device identification
 *   - Authentication (MFi challenge/response)
 *   - CarPlay session setup
 *   - ExternalAccessoryProtocol session for CarPlay data
 *
 * Message format (within link layer payload):
 *   uint16_t msg_length  — total message length
 *   uint16_t msg_id      — message identifier
 *   payload[]            — TLV-encoded parameters
 *
 * Key message IDs (iAP2 specification):
 */

/* Control session messages */
#define IAP2_MSG_IDENTIFICATION_INFO             0xAA00
#define IAP2_MSG_IDENTIFICATION_ACCEPTED         0xAA01
#define IAP2_MSG_IDENTIFICATION_REJECTED         0xAA02
#define IAP2_MSG_AUTH_CERT_REQUEST               0xAA10
#define IAP2_MSG_AUTH_CERT_RESPONSE              0xAA11
#define IAP2_MSG_AUTH_CHALLENGE_REQUEST           0xAA12
#define IAP2_MSG_AUTH_CHALLENGE_RESPONSE          0xAA13
#define IAP2_MSG_START_EAP_SESSION               0xAA20
#define IAP2_MSG_STOP_EAP_SESSION                0xAA21
#define IAP2_MSG_EAP_SESSION_STARTED             0xAA22

/* Session IDs */
#define IAP2_SESSION_CONTROL   0x01
#define IAP2_SESSION_EAP       0x02

typedef struct {
    iap2_link_t *link;
    bool identified;
    bool authenticated;
    bool carplay_started;

    /* MFi auth state */
    uint8_t mfi_cert[1024];
    int mfi_cert_len;
} iap2_session_t;

/**
 * Initialize session layer on top of a link.
 */
int iap2_session_init(iap2_session_t *sess, iap2_link_t *link);

/**
 * Send IdentificationInformation message.
 * Declares device name, supported features (CarPlay), etc.
 */
int iap2_session_send_identification(iap2_session_t *sess);

/**
 * Handle an incoming session message (called from link rx callback).
 */
int iap2_session_handle_message(iap2_session_t *sess,
                                 uint16_t msg_id,
                                 const uint8_t *payload, size_t payload_len);

/**
 * Send MFi certificate response.
 */
int iap2_session_send_auth_cert(iap2_session_t *sess);

/**
 * Send MFi challenge response (signed by MFi chip).
 */
int iap2_session_send_auth_response(iap2_session_t *sess,
                                     const uint8_t *challenge, size_t challenge_len);
