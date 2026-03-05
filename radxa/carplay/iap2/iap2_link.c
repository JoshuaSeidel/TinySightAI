#include "iap2_link.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

uint8_t iap2_checksum(const uint8_t *header, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += header[i];
    return 0x100 - sum;
}

static int send_raw(iap2_link_t *link, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(link->transport_fd, data + sent, len - sent);
        if (n <= 0) {
            perror("iap2_link: send");
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int build_and_send(iap2_link_t *link, uint8_t ctl, uint8_t seq,
                           uint8_t ack, uint8_t session_id,
                           const uint8_t *payload, size_t payload_len)
{
    uint16_t total_len = IAP2_HEADER_SIZE + payload_len;
    uint8_t header[IAP2_HEADER_SIZE];

    header[0] = IAP2_SYNC_BYTE1;
    header[1] = IAP2_SYNC_BYTE2;
    header[2] = (total_len >> 8) & 0xFF;
    header[3] = total_len & 0xFF;
    header[4] = ctl;
    header[5] = seq;
    header[6] = ack;
    header[7] = session_id;
    header[8] = iap2_checksum(header, 8);

    if (send_raw(link, header, IAP2_HEADER_SIZE) < 0)
        return -1;

    if (payload && payload_len > 0) {
        if (send_raw(link, payload, payload_len) < 0)
            return -1;
    }

    return 0;
}

int iap2_link_init(iap2_link_t *link, int transport_fd,
                    iap2_link_rx_cb_t rx_cb, void *ctx)
{
    memset(link, 0, sizeof(*link));
    link->transport_fd = transport_fd;
    link->rx_cb = rx_cb;
    link->rx_ctx = ctx;
    link->max_outstanding = 4;
    link->max_packet_len = 1024;
    link->synced = false;
    return 0;
}

int iap2_link_send_syn(iap2_link_t *link)
{
    /*
     * SYN payload: link negotiation parameters
     * version(1) + max_outstanding(1) + max_packet_len(2) + retransmit_timeout(2)
     */
    uint8_t syn_payload[6] = {
        0x01,  /* version */
        link->max_outstanding,
        (link->max_packet_len >> 8) & 0xFF,
        link->max_packet_len & 0xFF,
        0x00, 0x64,  /* retransmit timeout: 100ms */
    };

    printf("iap2_link: sending SYN (seq=%d)\n", link->local_seq);
    return build_and_send(link, IAP2_CTL_SYN, link->local_seq, 0, 0,
                          syn_payload, sizeof(syn_payload));
}

int iap2_link_send_ack(iap2_link_t *link, uint8_t ack_seq)
{
    return build_and_send(link, IAP2_CTL_ACK, link->local_seq, ack_seq, 0,
                          NULL, 0);
}

int iap2_link_send_data(iap2_link_t *link, uint8_t session_id,
                         const uint8_t *payload, size_t payload_len)
{
    int ret = build_and_send(link, 0x00, link->local_seq, link->remote_seq,
                              session_id, payload, payload_len);
    if (ret == 0)
        link->local_seq++;
    return ret;
}

int iap2_link_process(iap2_link_t *link)
{
    /* Poll with 500ms timeout so callers can check shutdown flags */
    struct pollfd pfd = { .fd = link->transport_fd, .events = POLLIN };
    int pret = poll(&pfd, 1, 500);
    if (pret == 0) return 0;          /* timeout — no data */
    if (pret < 0) {
        if (errno == EINTR) return 0;
        perror("iap2_link: poll");
        return -1;
    }

    /*
     * Assemble working buffer: carry-forward bytes from previous call
     * followed by freshly read bytes.
     */
    uint8_t buf[4096];
    size_t buf_len = 0;

    if (link->carry_len > 0) {
        memcpy(buf, link->carry_buf, link->carry_len);
        buf_len = link->carry_len;
        link->carry_len = 0;
    }

    ssize_t n = read(link->transport_fd, buf + buf_len, sizeof(buf) - buf_len);
    if (n <= 0) {
        if (n == 0) printf("iap2_link: connection closed\n");
        else if (errno != EAGAIN) perror("iap2_link: read");
        return (n == 0 || errno != EAGAIN) ? -1 : 0;
    }
    buf_len += (size_t)n;

    /* Parse packets from buffer */
    size_t pos = 0;
    while (pos + IAP2_HEADER_SIZE <= buf_len) {
        /* Find sync bytes */
        if (buf[pos] != IAP2_SYNC_BYTE1 || buf[pos + 1] != IAP2_SYNC_BYTE2) {
            pos++;
            continue;
        }

        uint16_t pkt_len = (buf[pos + 2] << 8) | buf[pos + 3];
        if (pos + pkt_len > buf_len) break; /* incomplete packet — carry forward */

        /* Verify checksum */
        uint8_t expected_cksum = iap2_checksum(buf + pos, 8);
        if (buf[pos + 8] != expected_cksum) {
            printf("iap2_link: bad checksum (got 0x%02X, expected 0x%02X)\n",
                   buf[pos + 8], expected_cksum);
            pos++;
            continue;
        }

        /* Parse header */
        iap2_packet_t pkt = {
            .length = pkt_len,
            .ctl = buf[pos + 4],
            .seq = buf[pos + 5],
            .ack = buf[pos + 6],
            .session_id = buf[pos + 7],
            .payload = (pkt_len > IAP2_HEADER_SIZE) ? buf + pos + IAP2_HEADER_SIZE : NULL,
            .payload_len = (pkt_len > IAP2_HEADER_SIZE) ? pkt_len - IAP2_HEADER_SIZE : 0,
        };

        /* Handle SYN/ACK at link level */
        if (pkt.ctl & IAP2_CTL_SYN) {
            printf("iap2_link: received SYN (seq=%d)\n", pkt.seq);
            link->remote_seq = pkt.seq;
            link->synced = true;
            /* Send SYN+ACK */
            build_and_send(link, IAP2_CTL_SYN | IAP2_CTL_ACK,
                          link->local_seq, pkt.seq, 0,
                          pkt.payload, pkt.payload_len);
            link->local_seq++;
        } else if (pkt.ctl & IAP2_CTL_ACK) {
            /* ACK received */
            link->remote_seq = pkt.seq;
        } else if (pkt.ctl & IAP2_CTL_RST) {
            printf("iap2_link: received RST\n");
            link->synced = false;
            return -1;
        } else {
            /* Data packet */
            link->remote_seq = pkt.seq;
            iap2_link_send_ack(link, pkt.seq);

            if (link->rx_cb)
                link->rx_cb(&pkt, link->rx_ctx);
        }

        pos += pkt_len;
    }

    /* Save any remaining incomplete data for next call */
    if (pos < buf_len) {
        size_t leftover = buf_len - pos;
        if (leftover > sizeof(link->carry_buf)) {
            fprintf(stderr, "iap2_link: carry buffer overflow (%zu bytes)\n", leftover);
            leftover = sizeof(link->carry_buf);
        }
        memmove(link->carry_buf, buf + pos, leftover);
        link->carry_len = leftover;
    }

    return 0;
}
