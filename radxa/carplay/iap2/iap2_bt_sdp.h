#pragma once

#include <stdint.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

/*
 * iAP2 Bluetooth SDP Service Registration
 *
 * Registers SDP service records for:
 *   - iAP2 (iPod Accessory Protocol 2)
 *   - CarPlay
 *   - Hands-Free Audio Gateway (HFP-AG, some head units require this)
 *
 * iAP2 UUID:    00000000-deca-fade-deca-deafdecacafe
 * CarPlay UUID: 2D8D2466-E14D-451C-88BC-7301ABEA291A
 * HFP-AG UUID:  0000111F-0000-1000-8000-00805F9B34FB (standard BT)
 *
 * RFCOMM channel 23 is the conventional iAP2 channel, but any free
 * channel can be used — the iPhone discovers it via SDP.
 */

#define IAP2_SDP_RFCOMM_CHANNEL  23

typedef struct {
    sdp_session_t *sdp_session;   /* Connection to local bluetoothd */
    sdp_record_t  *iap2_record;   /* iAP2 service record handle */
    sdp_record_t  *carplay_record;/* CarPlay service record handle */
    sdp_record_t  *hfp_record;    /* HFP-AG service record handle */
    uint8_t        rfcomm_channel;/* RFCOMM channel in use */
} iap2_sdp_t;

/**
 * Connect to the local BlueZ SDP daemon and register all service records.
 *
 * rfcomm_channel: the RFCOMM channel on which iap2_bt_transport listens.
 * Returns 0 on success, -1 on failure.
 */
int iap2_sdp_register(iap2_sdp_t *sdp, uint8_t rfcomm_channel);

/**
 * Unregister all service records and disconnect from SDP daemon.
 */
void iap2_sdp_unregister(iap2_sdp_t *sdp);
