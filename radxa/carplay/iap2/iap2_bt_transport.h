#pragma once

#include <stdint.h>
#include <stddef.h>
#include "iap2_bt_sdp.h"

/*
 * iAP2 Bluetooth RFCOMM Transport
 *
 * Manages the BlueZ RFCOMM socket used to carry iAP2 frames over Bluetooth.
 * Sits below the iAP2 link layer — provides a file descriptor that
 * iap2_link_init() can use as transport_fd.
 *
 * Sequence:
 *   1. iap2_bt_init()   — open adapter, set discoverable/pairable,
 *                          register SDP records, bind RFCOMM listen socket
 *   2. iap2_bt_accept() — block until iPhone connects, return data fd
 *   3. iap2_bt_read() / iap2_bt_write() — pass to iap2_link layer
 *   4. iap2_bt_close()  — close one connection
 *   5. iap2_bt_cleanup()— deregister SDP, close adapter resources
 *
 * Device class 0x000408:
 *   Major service: Audio (0x200000 would be audio service class, but the
 *   CoD device class field 0x000408 = Major: Audio/Video, Minor: Car audio)
 */

#define IAP2_BT_COD  0x000408   /* Car audio device class */

typedef struct {
    int          listen_fd;   /* RFCOMM listen socket */
    uint8_t      rfcomm_channel;
    iap2_sdp_t   sdp;         /* SDP registration state */
    char         adapter[18]; /* BT address string, e.g. "00:11:22:33:44:55" */
} iap2_bt_t;

/**
 * Initialize the Bluetooth transport.
 *
 * adapter: BT adapter address string ("00:11:22:33:44:55") or NULL for any.
 * Sets the adapter discoverable and pairable, configures the device class,
 * registers SDP records, and binds an RFCOMM listen socket.
 *
 * Returns 0 on success, -1 on failure.
 */
int iap2_bt_init(iap2_bt_t *bt, const char *adapter);

/**
 * Accept an incoming RFCOMM connection from an iPhone.
 *
 * Blocks until a connection arrives.
 * Returns a connected socket fd on success, -1 on failure.
 * The caller passes this fd to iap2_link_init() as transport_fd.
 */
int iap2_bt_accept(iap2_bt_t *bt);

/**
 * Read up to len bytes from an RFCOMM connection fd.
 * Retries on EINTR. Returns bytes read, 0 on close, -1 on error.
 */
ssize_t iap2_bt_read(int fd, uint8_t *buf, size_t len);

/**
 * Write exactly len bytes to an RFCOMM connection fd.
 * Retries on partial writes and EINTR.
 * Returns 0 on success, -1 on error.
 */
int iap2_bt_write(int fd, const uint8_t *buf, size_t len);

/**
 * Close a single RFCOMM connection fd returned by iap2_bt_accept().
 */
void iap2_bt_close(int fd);

/**
 * Unregister SDP records and release all adapter resources.
 * Does not close individual connection fds — call iap2_bt_close() first.
 */
void iap2_bt_cleanup(iap2_bt_t *bt);
