/*
 * aa_video_output.c — Send composited H.264 frames back to aa-proxy
 *
 * Design:
 *
 *   A background reconnect thread loops attempting to connect to
 *   /tmp/aa-video-out.sock (aa-proxy is the server).  Once connected,
 *   it stays connected until aa-proxy closes the socket, then retries.
 *
 *   aa_video_output_send_frame() writes a length-prefixed frame to the
 *   socket under a mutex.  If g_fd < 0 (not connected) the frame is
 *   dropped immediately without blocking.
 *
 *   The reconnect thread uses a short sleep between attempts so it does
 *   not spin aggressively when aa-proxy is not yet up.
 */
#include "aa_video_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ---- Internal state ---- */

static int             g_fd       = -1;   /* connected socket to aa-proxy   */
static volatile int    g_running  = 0;
static pthread_t       g_thread;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Reconnect thread ---- */

static void *aa_video_out_reconnect_thread(void *arg)
{
    (void)arg;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, AA_VIDEO_OUT_SOCK_PATH, sizeof(addr.sun_path) - 1);

    while (g_running) {
        pthread_mutex_lock(&g_send_lock);
        int already_connected = (g_fd >= 0);
        pthread_mutex_unlock(&g_send_lock);

        if (already_connected) {
            /* Poll: wait a bit, then check again */
            usleep(500000);  /* 500 ms */
            continue;
        }

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("aa_video_output: socket");
            usleep(2000000);  /* 2 s retry */
            continue;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            /* aa-proxy not yet listening — retry silently */
            close(fd);
            usleep(1000000);  /* 1 s retry */
            continue;
        }

        printf("aa_video_output: connected to aa-proxy (%s)\n",
               AA_VIDEO_OUT_SOCK_PATH);

        pthread_mutex_lock(&g_send_lock);
        g_fd = fd;
        pthread_mutex_unlock(&g_send_lock);

        /* Stay here until the connection is closed by the remote side.
         * We detect disconnects in aa_video_output_send_frame() which
         * clears g_fd; wait for that. */
        while (g_running) {
            pthread_mutex_lock(&g_send_lock);
            int still_open = (g_fd >= 0);
            pthread_mutex_unlock(&g_send_lock);
            if (!still_open) break;
            usleep(200000);  /* 200 ms */
        }
    }

    return NULL;
}

/* ---- Public API ---- */

int aa_video_output_init(void)
{
    g_running = 1;
    g_fd      = -1;

    if (pthread_create(&g_thread, NULL, aa_video_out_reconnect_thread, NULL) != 0) {
        perror("aa_video_output: pthread_create");
        g_running = 0;
        return -1;
    }

    printf("aa_video_output: output channel initialised (connecting to %s)\n",
           AA_VIDEO_OUT_SOCK_PATH);
    return 0;
}

int aa_video_output_send_frame(const uint8_t *data, size_t len)
{
    if (!data || len == 0)
        return 0;

    pthread_mutex_lock(&g_send_lock);

    if (g_fd < 0) {
        /* Not connected — drop frame silently */
        pthread_mutex_unlock(&g_send_lock);
        return 0;
    }

    /* Build 4-byte big-endian length header */
    uint8_t hdr[4];
    hdr[0] = (uint8_t)((len >> 24) & 0xFF);
    hdr[1] = (uint8_t)((len >> 16) & 0xFF);
    hdr[2] = (uint8_t)((len >>  8) & 0xFF);
    hdr[3] = (uint8_t)( len        & 0xFF);

    /* Use iovec to avoid an extra malloc/copy */
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len  = 4;
    iov[1].iov_base = (void *)data;
    iov[1].iov_len  = len;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov    = iov;
    msg.msg_iovlen = 2;

    ssize_t sent = sendmsg(g_fd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        /* Connection broken — close and let reconnect thread retry */
        fprintf(stderr, "aa_video_output: send failed (%s), disconnecting\n",
                strerror(errno));
        close(g_fd);
        g_fd = -1;
        pthread_mutex_unlock(&g_send_lock);
        return -1;
    }

    pthread_mutex_unlock(&g_send_lock);
    return 0;
}

void aa_video_output_destroy(void)
{
    g_running = 0;

    pthread_mutex_lock(&g_send_lock);
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    pthread_mutex_unlock(&g_send_lock);

    pthread_join(g_thread, NULL);
    printf("aa_video_output: destroyed\n");
}
