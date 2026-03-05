#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * iAP2 Link Layer
 *
 * Handles framing, checksums, sequence numbers, and retransmission
 * for the iAP2 protocol between the iPhone and our device.
 *
 * Frame format:
 *   0xFF 0x5A           — sync bytes
 *   uint16_t length     — total packet length (big-endian)
 *   uint8_t  ctl        — control byte (SYN, ACK, EAK, RST, SLP)
 *   uint8_t  seq        — sequence number
 *   uint8_t  ack        — acknowledgment number
 *   uint8_t  session_id — session identifier
 *   uint8_t  checksum   — header checksum
 *   payload[]           — session layer data
 *
 * Reference: wiomoc.de iAP2 research
 */

#define IAP2_SYNC_BYTE1  0xFF
#define IAP2_SYNC_BYTE2  0x5A
#define IAP2_HEADER_SIZE 9

/* Control byte flags */
#define IAP2_CTL_SYN     0x80  /* Synchronize */
#define IAP2_CTL_ACK     0x40  /* Acknowledge */
#define IAP2_CTL_EAK     0x20  /* Extended Acknowledge */
#define IAP2_CTL_RST     0x10  /* Reset */
#define IAP2_CTL_SLP     0x08  /* Sleep */

typedef struct {
    uint16_t length;
    uint8_t  ctl;
    uint8_t  seq;
    uint8_t  ack;
    uint8_t  session_id;
    const uint8_t *payload;
    size_t   payload_len;
} iap2_packet_t;

typedef void (*iap2_link_rx_cb_t)(const iap2_packet_t *pkt, void *ctx);

typedef struct {
    int transport_fd;         /* BT RFCOMM or TCP socket */
    uint8_t local_seq;
    uint8_t remote_seq;
    uint8_t max_outstanding;
    uint16_t max_packet_len;
    iap2_link_rx_cb_t rx_cb;
    void *rx_ctx;
    bool synced;

    /* Carry-forward buffer for TCP stream reassembly.
     * Incomplete packets at the end of a read() are saved here
     * and prepended to the next read(). */
    uint8_t carry_buf[4096];
    size_t  carry_len;
} iap2_link_t;

/**
 * Initialize link layer.
 */
int iap2_link_init(iap2_link_t *link, int transport_fd,
                    iap2_link_rx_cb_t rx_cb, void *ctx);

/**
 * Send a SYN packet to initiate link negotiation.
 */
int iap2_link_send_syn(iap2_link_t *link);

/**
 * Send an ACK for a received packet.
 */
int iap2_link_send_ack(iap2_link_t *link, uint8_t ack_seq);

/**
 * Send a data packet with payload.
 */
int iap2_link_send_data(iap2_link_t *link, uint8_t session_id,
                         const uint8_t *payload, size_t payload_len);

/**
 * Read and process incoming data. Calls rx_cb for complete packets.
 * Returns 0 on success, -1 on connection error.
 */
int iap2_link_process(iap2_link_t *link);

/**
 * Compute iAP2 header checksum.
 */
uint8_t iap2_checksum(const uint8_t *header, size_t len);
