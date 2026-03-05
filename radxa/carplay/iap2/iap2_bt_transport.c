#include "iap2_bt_transport.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

/* BlueZ kernel headers */
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Open a raw HCI socket to the first (or specified) local adapter and
 * configure discoverable + pairable mode, plus device class.
 *
 * adapter_addr: dotted BT address string or NULL/"" to use the first adapter.
 * Returns a bound HCI socket fd, or -1 on failure.
 */
static int hci_configure_adapter(const char *adapter_addr, uint8_t out_addr[6])
{
    int dev_id;

    if (adapter_addr && adapter_addr[0] != '\0') {
        bdaddr_t ba;
        str2ba(adapter_addr, &ba);
        dev_id = hci_get_route(&ba);
    } else {
        dev_id = hci_get_route(NULL); /* first available adapter */
    }

    if (dev_id < 0) {
        fprintf(stderr, "iap2: no Bluetooth adapter found\n");
        return -1;
    }

    int hci_fd = hci_open_dev(dev_id);
    if (hci_fd < 0) {
        fprintf(stderr, "iap2: hci_open_dev(%d) failed: %s\n",
                dev_id, strerror(errno));
        return -1;
    }

    /* Retrieve adapter address for later logging */
    struct hci_dev_info di;
    memset(&di, 0, sizeof(di));
    di.dev_id = dev_id;
    if (ioctl(hci_fd, HCIGETDEVINFO, (void *)&di) == 0) {
        memcpy(out_addr, di.bdaddr.b, 6);
        char addr_str[18];
        ba2str(&di.bdaddr, addr_str);
        printf("iap2: using BT adapter hci%d (%s)\n", dev_id, addr_str);
    }

    /* Set scan mode: inquiry + page (discoverable + connectable) */
    struct hci_request rq;
    write_scan_enable_cp scan_cp;
    uint8_t scan_status;

    memset(&scan_cp, 0, sizeof(scan_cp));
    scan_cp.scan_enable = SCAN_INQUIRY | SCAN_PAGE;

    memset(&rq, 0, sizeof(rq));
    rq.ogf    = OGF_HOST_CTL;
    rq.ocf    = OCF_WRITE_SCAN_ENABLE;
    rq.cparam = &scan_cp;
    rq.clen   = WRITE_SCAN_ENABLE_CP_SIZE;
    rq.rparam = &scan_status;
    rq.rlen   = 1;

    if (hci_send_req(hci_fd, &rq, 2000) < 0) {
        fprintf(stderr, "iap2: failed to set scan mode: %s\n", strerror(errno));
        /* Non-fatal — carry on */
    } else {
        printf("iap2: BT adapter set to discoverable+connectable\n");
    }

    /* Set device class to 0x000408 (car audio) */
    write_class_of_dev_cp cod_cp;
    uint8_t cod_status;
    cod_cp.dev_class[0] = (IAP2_BT_COD >> 0)  & 0xFF;
    cod_cp.dev_class[1] = (IAP2_BT_COD >> 8)  & 0xFF;
    cod_cp.dev_class[2] = (IAP2_BT_COD >> 16) & 0xFF;

    memset(&rq, 0, sizeof(rq));
    rq.ogf    = OGF_HOST_CTL;
    rq.ocf    = OCF_WRITE_CLASS_OF_DEV;
    rq.cparam = &cod_cp;
    rq.clen   = WRITE_CLASS_OF_DEV_CP_SIZE;
    rq.rparam = &cod_status;
    rq.rlen   = 1;

    if (hci_send_req(hci_fd, &rq, 2000) < 0) {
        fprintf(stderr, "iap2: failed to set device class: %s\n", strerror(errno));
        /* Non-fatal */
    } else {
        printf("iap2: device class set to 0x%06X (car audio)\n", IAP2_BT_COD);
    }

    /*
     * We do not close hci_fd here; return it so the caller can hold it open
     * for the lifetime of the adapter (scan mode reset on close).
     */
    return hci_fd;
}

/*
 * Bind and listen on an RFCOMM server socket.
 * channel: RFCOMM channel number (1-30).
 * Returns bound listening socket fd, or -1 on failure.
 */
static int rfcomm_listen(uint8_t channel)
{
    int sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (sock < 0) {
        fprintf(stderr, "iap2: socket(RFCOMM) failed: %s\n", strerror(errno));
        return -1;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_rc addr;
    memset(&addr, 0, sizeof(addr));
    addr.rc_family  = AF_BLUETOOTH;
    addr.rc_bdaddr  = *BDADDR_ANY;
    addr.rc_channel = channel;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "iap2: bind(RFCOMM ch %d) failed: %s\n",
                channel, strerror(errno));
        close(sock);
        return -1;
    }

    if (listen(sock, 1) < 0) {
        fprintf(stderr, "iap2: listen(RFCOMM) failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    printf("iap2: RFCOMM listening on channel %d\n", channel);
    return sock;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int iap2_bt_init(iap2_bt_t *bt, const char *adapter)
{
    memset(bt, 0, sizeof(*bt));
    bt->listen_fd = -1;

    if (adapter && adapter[0] != '\0') {
        strncpy(bt->adapter, adapter, sizeof(bt->adapter) - 1);
    }

    /*
     * Step 1: Configure HCI adapter (discoverable, CoD).
     * We keep the HCI fd in listen_fd temporarily, then replace it with
     * the RFCOMM socket.  Actually we open an HCI fd just for configuration
     * and close it — the RFCOMM socket is what we keep.
     */
    uint8_t hci_addr[6];
    int hci_fd = hci_configure_adapter(adapter, hci_addr);
    if (hci_fd < 0) {
        fprintf(stderr, "iap2: HCI adapter configuration failed\n");
        return -1;
    }
    /* Close HCI socket after configuration — RFCOMM socket is independent */
    close(hci_fd);

    /* Step 2: Register SDP records */
    bt->rfcomm_channel = IAP2_SDP_RFCOMM_CHANNEL;
    if (iap2_sdp_register(&bt->sdp, bt->rfcomm_channel) < 0) {
        fprintf(stderr, "iap2: SDP registration failed\n");
        return -1;
    }

    /* Step 3: Create RFCOMM listen socket */
    bt->listen_fd = rfcomm_listen(bt->rfcomm_channel);
    if (bt->listen_fd < 0) {
        fprintf(stderr, "iap2: RFCOMM listen socket creation failed\n");
        iap2_sdp_unregister(&bt->sdp);
        return -1;
    }

    printf("iap2: BT transport initialized, waiting for iPhone on RFCOMM ch %d\n",
           bt->rfcomm_channel);
    return 0;
}

int iap2_bt_accept(iap2_bt_t *bt)
{
    struct sockaddr_rc peer_addr;
    socklen_t addr_len = sizeof(peer_addr);

    printf("iap2: waiting for incoming BT connection...\n");

    int conn_fd = accept(bt->listen_fd,
                         (struct sockaddr *)&peer_addr, &addr_len);
    if (conn_fd < 0) {
        if (errno == EINTR) {
            /* Interrupted by signal — caller can retry */
            return -1;
        }
        fprintf(stderr, "iap2: accept(RFCOMM) failed: %s\n", strerror(errno));
        return -1;
    }

    char peer_str[18];
    ba2str(&peer_addr.rc_bdaddr, peer_str);
    printf("iap2: iPhone connected via RFCOMM from %s\n", peer_str);

    return conn_fd;
}

ssize_t iap2_bt_read(int fd, uint8_t *buf, size_t len)
{
    ssize_t n;
    do {
        n = read(fd, buf, len);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        fprintf(stderr, "iap2: RFCOMM read error: %s\n", strerror(errno));
    } else if (n == 0) {
        printf("iap2: RFCOMM connection closed by peer\n");
    }
    return n;
}

int iap2_bt_write(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n;
        do {
            n = write(fd, buf + sent, len - sent);
        } while (n < 0 && errno == EINTR);

        if (n <= 0) {
            fprintf(stderr, "iap2: RFCOMM write error: %s\n", strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

void iap2_bt_close(int fd)
{
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        printf("iap2: RFCOMM connection closed\n");
    }
}

void iap2_bt_cleanup(iap2_bt_t *bt)
{
    if (bt->listen_fd >= 0) {
        close(bt->listen_fd);
        bt->listen_fd = -1;
        printf("iap2: RFCOMM listen socket closed\n");
    }
    iap2_sdp_unregister(&bt->sdp);
    printf("iap2: BT transport cleaned up\n");
}
