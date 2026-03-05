#pragma once

/*
 * airplay_mdns.h — mDNS/Bonjour Advertisement via Avahi
 *
 * Registers the _airplay._tcp service so that iPhones on the local network
 * can discover our AirPlay receiver.
 *
 * Required TXT records for AirPlay screen mirroring:
 *
 *   deviceid  = AA:BB:CC:DD:EE:FF   (MAC address — must be unique)
 *   features  = 0x5A7FFFF7,0x1E     (capability bitmask)
 *               Bit meanings (from RPiPlay / homebridge-airplay2):
 *                 bit 0   : Video
 *                 bit 1   : Photo
 *                 bit 2   : VideoFairPlay
 *                 bit 3   : VideoVolumeCtrl
 *                 bit 4   : VideoHTTPLiveStreams
 *                 bit 5   : Slideshow
 *                 bit 7   : Screen
 *                 bit 9   : Audio
 *                 bit 11  : Audio redundant
 *                 bit 14  : FPSAPDSecondScreenSetup
 *                 bit 17  : AudioMetaDataControls
 *                 bit 18  : PhotoCaching
 *                 bit 19  : Authentication4
 *                 bit 30  : HasUnifiedAdvertiserInfo
 *   model     = AppleTV3,2          (mimic Apple TV — widest iOS compatibility)
 *   srcvers   = 220.68              (AirPlay protocol version)
 *   vv        = 2                   (required for iOS 14+)
 *   pk        = <32-byte Ed25519 public key as hex>  (for pair-verify)
 *   pi        = <UUID>              (persistent identifier)
 *
 * Service:  _airplay._tcp
 * Port:     7000
 *
 * Dependencies: libavahi-client-dev, libavahi-common-dev
 *
 * Avahi usage pattern:
 *   1. Create AvahiSimplePoll (event loop)
 *   2. Create AvahiClient
 *   3. On client state AVAHI_CLIENT_S_RUNNING → create entry group
 *   4. Add service with avahi_entry_group_add_service_strlst()
 *   5. Commit the group
 *   6. Run poll loop in a background thread
 */

#include <stdint.h>
#include <stdbool.h>

/* Opaque handle — details in .c */
typedef struct airplay_mdns_ctx airplay_mdns_ctx_t;

/*
 * Register the AirPlay service on the local network.
 *
 * Parameters:
 *   mac         — device MAC, "AA:BB:CC:DD:EE:FF"
 *   name        — service name shown in iOS AirPlay picker
 *   ed25519_pub — 32-byte server public key (for pk TXT record)
 *   port        — TCP port (usually 7000)
 *
 * Returns allocated context on success, NULL on error.
 * The registration runs in a background thread managed by Avahi.
 */
airplay_mdns_ctx_t *airplay_mdns_register(const char *mac,
                                           const char *name,
                                           const uint8_t ed25519_pub[32],
                                           uint16_t port);

/*
 * Unregister the service and free the context.
 */
void airplay_mdns_unregister(airplay_mdns_ctx_t *ctx);
