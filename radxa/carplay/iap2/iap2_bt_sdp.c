#include "iap2_bt_sdp.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

/*
 * iAP2 UUID: 00000000-deca-fade-deca-deafdecacafe
 * Stored as 128-bit little-endian for BlueZ uuid_t.
 */
static const uint8_t IAP2_UUID_BYTES[16] = {
    0xfe, 0xca, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xde,
    0xde, 0xfa, 0xca, 0xde, 0x00, 0x00, 0x00, 0x00
};

/*
 * CarPlay UUID: 2D8D2466-E14D-451C-88BC-7301ABEA291A
 * Little-endian byte order for BlueZ.
 */
static const uint8_t CARPLAY_UUID_BYTES[16] = {
    0x1a, 0x29, 0xea, 0xab, 0x01, 0x73, 0xbc, 0x88,
    0x1c, 0x45, 0x4d, 0xe1, 0x66, 0x24, 0x8d, 0x2d
};

/* HFP Audio Gateway UUID: 0x111F (standard 16-bit) */
#define HFP_AG_UUID16  0x111F

/* GenericAudio service class: 0x1203 */
#define GENERIC_AUDIO_UUID16 0x1203

/* Human-readable service names */
#define IAP2_SERVICE_NAME    "iAP2 Accessory"
#define CARPLAY_SERVICE_NAME "CarPlay"
#define HFP_AG_SERVICE_NAME  "Hands-Free Audio Gateway"

/* -------------------------------------------------------------------------- */

static sdp_record_t *build_iap2_record(uint8_t rfcomm_channel)
{
    sdp_record_t *record = sdp_record_alloc();
    if (!record) {
        fprintf(stderr, "iap2: sdp_record_alloc failed\n");
        return NULL;
    }

    /* Service class: iAP2 128-bit UUID */
    uuid_t iap2_uuid;
    sdp_uuid128_create(&iap2_uuid, IAP2_UUID_BYTES);

    sdp_list_t *svc_class = sdp_list_append(NULL, &iap2_uuid);
    if (sdp_set_service_classes(record, svc_class) < 0) {
        fprintf(stderr, "iap2: sdp_set_service_classes failed for iap2\n");
        sdp_list_free(svc_class, NULL);
        sdp_record_free(record);
        return NULL;
    }
    sdp_list_free(svc_class, NULL);

    /* Protocol descriptor: L2CAP + RFCOMM */
    uuid_t l2cap_uuid, rfcomm_uuid;
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);

    sdp_data_t *channel_data = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
    sdp_list_t *rfcomm_list  = sdp_list_append(NULL, &rfcomm_uuid);
    rfcomm_list = sdp_list_append(rfcomm_list, channel_data);

    sdp_list_t *l2cap_list = sdp_list_append(NULL, &l2cap_uuid);

    sdp_list_t *proto_list = sdp_list_append(NULL, l2cap_list);
    proto_list = sdp_list_append(proto_list, rfcomm_list);

    sdp_list_t *access_proto = sdp_list_append(NULL, proto_list);
    if (sdp_set_access_protos(record, access_proto) < 0) {
        fprintf(stderr, "iap2: sdp_set_access_protos failed for iap2\n");
        sdp_data_free(channel_data);
        sdp_list_free(rfcomm_list, NULL);
        sdp_list_free(l2cap_list, NULL);
        sdp_list_free(proto_list, NULL);
        sdp_list_free(access_proto, NULL);
        sdp_record_free(record);
        return NULL;
    }
    sdp_data_free(channel_data);
    sdp_list_free(rfcomm_list, NULL);
    sdp_list_free(l2cap_list, NULL);
    sdp_list_free(proto_list, NULL);
    sdp_list_free(access_proto, NULL);

    /* Browse group: public */
    uuid_t root_uuid;
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    sdp_list_t *browse_list = sdp_list_append(NULL, &root_uuid);
    sdp_set_browse_groups(record, browse_list);
    sdp_list_free(browse_list, NULL);

    /* Service name */
    sdp_set_info_attr(record, IAP2_SERVICE_NAME, "Custom", "iAP2 accessory service");

    return record;
}

static sdp_record_t *build_carplay_record(uint8_t rfcomm_channel)
{
    sdp_record_t *record = sdp_record_alloc();
    if (!record) {
        fprintf(stderr, "iap2: sdp_record_alloc failed for carplay\n");
        return NULL;
    }

    /* Service class: CarPlay 128-bit UUID */
    uuid_t cp_uuid;
    sdp_uuid128_create(&cp_uuid, CARPLAY_UUID_BYTES);

    sdp_list_t *svc_class = sdp_list_append(NULL, &cp_uuid);
    if (sdp_set_service_classes(record, svc_class) < 0) {
        fprintf(stderr, "iap2: sdp_set_service_classes failed for carplay\n");
        sdp_list_free(svc_class, NULL);
        sdp_record_free(record);
        return NULL;
    }
    sdp_list_free(svc_class, NULL);

    /* Protocol descriptor: L2CAP + RFCOMM (same channel, iAP2 carries CarPlay) */
    uuid_t l2cap_uuid, rfcomm_uuid;
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);

    sdp_data_t *channel_data = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
    sdp_list_t *rfcomm_list  = sdp_list_append(NULL, &rfcomm_uuid);
    rfcomm_list = sdp_list_append(rfcomm_list, channel_data);
    sdp_list_t *l2cap_list   = sdp_list_append(NULL, &l2cap_uuid);

    sdp_list_t *proto_list  = sdp_list_append(NULL, l2cap_list);
    proto_list = sdp_list_append(proto_list, rfcomm_list);

    sdp_list_t *access_proto = sdp_list_append(NULL, proto_list);
    if (sdp_set_access_protos(record, access_proto) < 0) {
        fprintf(stderr, "iap2: sdp_set_access_protos failed for carplay\n");
        sdp_data_free(channel_data);
        sdp_list_free(rfcomm_list, NULL);
        sdp_list_free(l2cap_list, NULL);
        sdp_list_free(proto_list, NULL);
        sdp_list_free(access_proto, NULL);
        sdp_record_free(record);
        return NULL;
    }
    sdp_data_free(channel_data);
    sdp_list_free(rfcomm_list, NULL);
    sdp_list_free(l2cap_list, NULL);
    sdp_list_free(proto_list, NULL);
    sdp_list_free(access_proto, NULL);

    /* Browse group */
    uuid_t root_uuid;
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    sdp_list_t *browse_list = sdp_list_append(NULL, &root_uuid);
    sdp_set_browse_groups(record, browse_list);
    sdp_list_free(browse_list, NULL);

    sdp_set_info_attr(record, CARPLAY_SERVICE_NAME, "Custom", "Apple CarPlay service");

    return record;
}

static sdp_record_t *build_hfp_ag_record(void)
{
    sdp_record_t *record = sdp_record_alloc();
    if (!record) {
        fprintf(stderr, "iap2: sdp_record_alloc failed for hfp-ag\n");
        return NULL;
    }

    /* Service class: HandsfreeAudioGateway + GenericAudio */
    uuid_t hfp_uuid, audio_uuid;
    sdp_uuid16_create(&hfp_uuid, HFP_AG_UUID16);
    sdp_uuid16_create(&audio_uuid, GENERIC_AUDIO_UUID16);

    sdp_list_t *svc_class = sdp_list_append(NULL, &hfp_uuid);
    svc_class = sdp_list_append(svc_class, &audio_uuid);
    if (sdp_set_service_classes(record, svc_class) < 0) {
        fprintf(stderr, "iap2: sdp_set_service_classes failed for hfp-ag\n");
        sdp_list_free(svc_class, NULL);
        sdp_record_free(record);
        return NULL;
    }
    sdp_list_free(svc_class, NULL);

    /*
     * HFP-AG uses RFCOMM channel 2 by convention (though it does not carry
     * actual HFP traffic here — we only register the profile so that car
     * head units with strict profile requirements can discover it).
     */
    uint8_t hfp_channel = 2;
    uuid_t l2cap_uuid, rfcomm_uuid;
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);

    sdp_data_t *channel_data = sdp_data_alloc(SDP_UINT8, &hfp_channel);
    sdp_list_t *rfcomm_list  = sdp_list_append(NULL, &rfcomm_uuid);
    rfcomm_list = sdp_list_append(rfcomm_list, channel_data);
    sdp_list_t *l2cap_list   = sdp_list_append(NULL, &l2cap_uuid);

    sdp_list_t *proto_list  = sdp_list_append(NULL, l2cap_list);
    proto_list = sdp_list_append(proto_list, rfcomm_list);

    sdp_list_t *access_proto = sdp_list_append(NULL, proto_list);
    if (sdp_set_access_protos(record, access_proto) < 0) {
        fprintf(stderr, "iap2: sdp_set_access_protos failed for hfp-ag\n");
        sdp_data_free(channel_data);
        sdp_list_free(rfcomm_list, NULL);
        sdp_list_free(l2cap_list, NULL);
        sdp_list_free(proto_list, NULL);
        sdp_list_free(access_proto, NULL);
        sdp_record_free(record);
        return NULL;
    }
    sdp_data_free(channel_data);
    sdp_list_free(rfcomm_list, NULL);
    sdp_list_free(l2cap_list, NULL);
    sdp_list_free(proto_list, NULL);
    sdp_list_free(access_proto, NULL);

    /* Profile descriptor: HFP version 1.6 */
    uuid_t profile_uuid;
    sdp_uuid16_create(&profile_uuid, HFP_AG_UUID16);
    uint16_t hfp_version = 0x0106;
    sdp_profile_desc_t profile = { profile_uuid, hfp_version };
    sdp_list_t *profile_list = sdp_list_append(NULL, &profile);
    sdp_set_profile_descs(record, profile_list);
    sdp_list_free(profile_list, NULL);

    /* HFP network feature bit: no ability to reject calls */
    uint8_t network = 0x00;
    sdp_attr_add_new(record, SDP_ATTR_NET, SDP_UINT8, &network);

    /* HFP supported features bitmap (AG): basic set */
    uint16_t features = 0x0003; /* 3-way calling + echo cancel */
    sdp_attr_add_new(record, SDP_ATTR_SUPPORTED_FEATURES, SDP_UINT16, &features);

    /* Browse group */
    uuid_t root_uuid;
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    sdp_list_t *browse_list = sdp_list_append(NULL, &root_uuid);
    sdp_set_browse_groups(record, browse_list);
    sdp_list_free(browse_list, NULL);

    sdp_set_info_attr(record, HFP_AG_SERVICE_NAME, "Custom",
                      "Hands-Free Audio Gateway (stub)");

    return record;
}

/* -------------------------------------------------------------------------- */

int iap2_sdp_register(iap2_sdp_t *sdp, uint8_t rfcomm_channel)
{
    memset(sdp, 0, sizeof(*sdp));
    sdp->rfcomm_channel = rfcomm_channel;

    /* Connect to local bluetoothd SDP session */
    sdp->sdp_session = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
    if (!sdp->sdp_session) {
        fprintf(stderr, "iap2: sdp_connect failed: %s\n", strerror(errno));
        return -1;
    }

    /* Register iAP2 record */
    sdp->iap2_record = build_iap2_record(rfcomm_channel);
    if (!sdp->iap2_record) {
        fprintf(stderr, "iap2: failed to build iap2 SDP record\n");
        goto err_session;
    }
    if (sdp_record_register(sdp->sdp_session, sdp->iap2_record, 0) < 0) {
        fprintf(stderr, "iap2: sdp_record_register failed for iap2: %s\n",
                strerror(errno));
        goto err_iap2;
    }
    printf("iap2: registered iAP2 SDP record (channel %d)\n", rfcomm_channel);

    /* Register CarPlay record */
    sdp->carplay_record = build_carplay_record(rfcomm_channel);
    if (!sdp->carplay_record) {
        fprintf(stderr, "iap2: failed to build carplay SDP record\n");
        goto err_iap2;
    }
    if (sdp_record_register(sdp->sdp_session, sdp->carplay_record, 0) < 0) {
        fprintf(stderr, "iap2: sdp_record_register failed for carplay: %s\n",
                strerror(errno));
        goto err_carplay;
    }
    printf("iap2: registered CarPlay SDP record\n");

    /* Register HFP-AG record */
    sdp->hfp_record = build_hfp_ag_record();
    if (!sdp->hfp_record) {
        fprintf(stderr, "iap2: failed to build hfp-ag SDP record\n");
        goto err_carplay;
    }
    if (sdp_record_register(sdp->sdp_session, sdp->hfp_record, 0) < 0) {
        fprintf(stderr, "iap2: sdp_record_register failed for hfp-ag: %s\n",
                strerror(errno));
        goto err_hfp;
    }
    printf("iap2: registered HFP-AG SDP record\n");

    return 0;

err_hfp:
    sdp_record_free(sdp->hfp_record);
    sdp->hfp_record = NULL;
err_carplay:
    sdp_record_unregister(sdp->sdp_session, sdp->carplay_record);
    sdp_record_free(sdp->carplay_record);
    sdp->carplay_record = NULL;
err_iap2:
    sdp_record_unregister(sdp->sdp_session, sdp->iap2_record);
    sdp_record_free(sdp->iap2_record);
    sdp->iap2_record = NULL;
err_session:
    sdp_close(sdp->sdp_session);
    sdp->sdp_session = NULL;
    return -1;
}

void iap2_sdp_unregister(iap2_sdp_t *sdp)
{
    if (!sdp->sdp_session) return;

    if (sdp->hfp_record) {
        sdp_record_unregister(sdp->sdp_session, sdp->hfp_record);
        sdp_record_free(sdp->hfp_record);
        sdp->hfp_record = NULL;
        printf("iap2: unregistered HFP-AG SDP record\n");
    }
    if (sdp->carplay_record) {
        sdp_record_unregister(sdp->sdp_session, sdp->carplay_record);
        sdp_record_free(sdp->carplay_record);
        sdp->carplay_record = NULL;
        printf("iap2: unregistered CarPlay SDP record\n");
    }
    if (sdp->iap2_record) {
        sdp_record_unregister(sdp->sdp_session, sdp->iap2_record);
        sdp_record_free(sdp->iap2_record);
        sdp->iap2_record = NULL;
        printf("iap2: unregistered iAP2 SDP record\n");
    }

    sdp_close(sdp->sdp_session);
    sdp->sdp_session = NULL;
}
