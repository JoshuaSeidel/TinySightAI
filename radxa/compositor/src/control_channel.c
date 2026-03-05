/*
 * control_channel.c — Control channel for runtime compositor commands
 *
 * Runs a non-blocking poll loop over:
 *   - TCP server socket (port 5290)
 *   - Unix domain server socket (/tmp/compositor-control.sock)
 *   - Up to CONTROL_MAX_CLIENTS simultaneous connected clients
 *
 * Each client sends newline-terminated command strings; we accumulate
 * bytes into a per-client line buffer and dispatch when '\n' is found.
 *
 * MODE commands are forwarded to aa-proxy via /tmp/aa-control.sock so
 * aa-proxy can update its touch-remapping regions to match the new layout.
 * The forwarded line is the raw command string followed by '\n', e.g.:
 *   "MODE split_aa_cam\n"
 * aa-proxy reads lines from that socket and adjusts its MITM behaviour.
 * Connection to /tmp/aa-control.sock is maintained lazily — we reconnect
 * automatically if aa-proxy restarts.
 */
#include "control_channel.h"
#include "baby_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ---- aa-proxy control socket path ---- */
#define AA_CONTROL_SOCK_PATH  "/tmp/aa-control.sock"

/* ---- Internal state ---- */

#define LINE_BUF_SIZE 256

typedef struct {
    int  fd;
    char line_buf[LINE_BUF_SIZE];
    int  line_len;
} ctrl_client_t;

static int              g_tcp_server  = -1;
static int              g_unix_server = -1;
static ctrl_client_t    g_clients[CONTROL_MAX_CLIENTS];
static int              g_num_clients = 0;

static display_state_t *g_display = NULL;
static camera_t        *g_cam     = NULL;
static pthread_mutex_t *g_lock    = NULL;
static ir_control_cb    g_ir_cb   = NULL;

/* Current IR state for STATUS reporting */
static int g_ir_state = 0;  /* 0=off, 1=on, 2=auto */

/* ---- aa-proxy control socket (lazy connect, client role) ---- */

/*
 * g_aa_ctrl_fd: fd of a connected Unix socket to aa-proxy's control listener.
 * -1 when disconnected.  We try to (re)connect on every MODE command.
 */
static int g_aa_ctrl_fd = -1;

/*
 * Attempt to connect/reconnect to /tmp/aa-control.sock.
 * Non-blocking: if aa-proxy is not yet running, we just leave g_aa_ctrl_fd=-1.
 */
static void aa_ctrl_ensure_connected(void)
{
    if (g_aa_ctrl_fd >= 0)
        return; /* already connected */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, AA_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); /* aa-proxy not ready yet — silently skip */
        return;
    }

    g_aa_ctrl_fd = fd;
    printf("control: connected to aa-proxy control socket (%s)\n",
           AA_CONTROL_SOCK_PATH);
}

/*
 * Forward a MODE command line to aa-proxy.
 * `cmd` should be the full command as received (e.g. "MODE split_aa_cam").
 * We append '\n' and send.  On error we close g_aa_ctrl_fd so the next
 * MODE command triggers a reconnect attempt.
 */
static void aa_ctrl_forward(const char *cmd)
{
    aa_ctrl_ensure_connected();

    if (g_aa_ctrl_fd < 0)
        return; /* aa-proxy not reachable — skip silently */

    char line[256];
    int  n = snprintf(line, sizeof(line), "%s\n", cmd);
    if (n <= 0 || n >= (int)sizeof(line))
        return;

    ssize_t sent = send(g_aa_ctrl_fd, line, (size_t)n, MSG_NOSIGNAL);
    if (sent < 0) {
        fprintf(stderr, "control: aa-proxy control send failed (%s), disconnecting\n",
                strerror(errno));
        close(g_aa_ctrl_fd);
        g_aa_ctrl_fd = -1;
    }
}

/* ---- Helpers ---- */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_tcp_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("control: socket(tcp)"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    set_nonblocking(fd);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("control: bind(tcp)"); close(fd); return -1;
    }
    if (listen(fd, 4) < 0) {
        perror("control: listen(tcp)"); close(fd); return -1;
    }
    printf("control: TCP listener on port %d\n", port);
    return fd;
}

static int create_unix_server(const char *path)
{
    unlink(path); /* remove stale socket */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("control: socket(unix)"); return -1; }

    set_nonblocking(fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("control: bind(unix)"); close(fd); return -1;
    }
    if (listen(fd, 4) < 0) {
        perror("control: listen(unix)"); close(fd); return -1;
    }
    printf("control: Unix socket at %s\n", path);
    return fd;
}

static void client_add(int fd)
{
    if (g_num_clients >= CONTROL_MAX_CLIENTS) {
        fprintf(stderr, "control: client limit reached, refusing fd %d\n", fd);
        close(fd);
        return;
    }
    set_nonblocking(fd);
    ctrl_client_t *c = &g_clients[g_num_clients++];
    c->fd = fd;
    c->line_len = 0;
    memset(c->line_buf, 0, sizeof(c->line_buf));
    printf("control: client connected (fd=%d, total=%d)\n", fd, g_num_clients);
}

static void client_remove(int idx)
{
    printf("control: client disconnected (fd=%d)\n", g_clients[idx].fd);
    close(g_clients[idx].fd);
    /* Compact array */
    g_num_clients--;
    if (idx < g_num_clients)
        g_clients[idx] = g_clients[g_num_clients];
}

/* ---- Command dispatch ---- */

/* Returns a static string describing the current layout */
static const char *layout_name(layout_mode_t l)
{
    switch (l) {
    case LAYOUT_FULL_PRIMARY:     return "full_primary";
    case LAYOUT_FULL_CAMERA:      return "full_camera";
    case LAYOUT_SPLIT_LEFT_RIGHT: return "split";
    default:                      return "unknown";
    }
}

static const char *source_name(primary_source_t s)
{
    return (s == SOURCE_AA) ? "aa" : "carplay";
}

static const char *ir_name(int state)
{
    if (state == 0) return "off";
    if (state == 1) return "on";
    return "auto";
}

/*
 * Dispatch a single null-terminated command string.
 * Sends a reply to `reply_fd` if >= 0.
 */
static void dispatch_command(const char *cmd, int reply_fd)
{
    char reply[256] = {0};

    if (strncmp(cmd, "MODE ", 5) == 0) {
        const char *arg = cmd + 5;

        pthread_mutex_lock(g_lock);

        if (strcmp(arg, "cycle") == 0) {
            mode_cycle(g_display);
        } else if (strcmp(arg, "full_aa") == 0) {
            g_display->layout = LAYOUT_FULL_PRIMARY;
            g_display->source = SOURCE_AA;
        } else if (strcmp(arg, "full_carplay") == 0) {
            g_display->layout = LAYOUT_FULL_PRIMARY;
            g_display->source = SOURCE_CARPLAY;
        } else if (strcmp(arg, "full_camera") == 0) {
            g_display->layout = LAYOUT_FULL_CAMERA;
        } else if (strcmp(arg, "split_aa_cam") == 0) {
            g_display->layout = LAYOUT_SPLIT_LEFT_RIGHT;
            g_display->source = SOURCE_AA;
        } else if (strcmp(arg, "split_cp_cam") == 0) {
            g_display->layout = LAYOUT_SPLIT_LEFT_RIGHT;
            g_display->source = SOURCE_CARPLAY;
        } else {
            snprintf(reply, sizeof(reply), "ERR unknown mode: %s\n", arg);
            pthread_mutex_unlock(g_lock);
            goto send_reply;
        }

        snprintf(reply, sizeof(reply), "OK mode=%s source=%s\n",
                 layout_name(g_display->layout),
                 source_name(g_display->source));
        pthread_mutex_unlock(g_lock);

        /*
         * Forward the MODE command to aa-proxy so it can update its touch
         * remapping zones to match the new display layout.
         * We forward the raw cmd string (e.g. "MODE split_aa_cam").
         */
        aa_ctrl_forward(cmd);

    } else if (strncmp(cmd, "ZOOM ", 5) == 0) {
        const char *arg = cmd + 5;

        pthread_mutex_lock(g_lock);
        if (strcmp(arg, "in") == 0) {
            mode_zoom_in(g_display);
            if (g_cam) camera_set_zoom(g_cam, g_display->cam_zoom);
        } else if (strcmp(arg, "out") == 0) {
            mode_zoom_out(g_display);
            if (g_cam) camera_set_zoom(g_cam, g_display->cam_zoom);
        } else if (strcmp(arg, "reset") == 0) {
            g_display->cam_zoom = 1.0f;
            if (g_cam) camera_set_zoom(g_cam, 1.0f);
        } else {
            snprintf(reply, sizeof(reply), "ERR unknown zoom: %s\n", arg);
            pthread_mutex_unlock(g_lock);
            goto send_reply;
        }
        snprintf(reply, sizeof(reply), "OK zoom=%.2f\n", g_display->cam_zoom);
        pthread_mutex_unlock(g_lock);

    } else if (strncmp(cmd, "IR ", 3) == 0) {
        const char *arg = cmd + 3;
        int new_ir = -1;

        if (strcmp(arg, "on")   == 0) new_ir = 1;
        else if (strcmp(arg, "off")  == 0) new_ir = 0;
        else if (strcmp(arg, "auto") == 0) new_ir = 2;

        if (new_ir < 0) {
            snprintf(reply, sizeof(reply), "ERR unknown IR arg: %s\n", arg);
        } else {
            g_ir_state = new_ir;
            /* Pass mode string ("on"/"off"/"auto") to the IR callback so it
             * can write to /tmp/ir-mode for ir-led-control.sh to pick up. */
            if (g_ir_cb) g_ir_cb(ir_name(g_ir_state));
            snprintf(reply, sizeof(reply), "OK ir=%s\n", ir_name(g_ir_state));
        }

    } else if (strncmp(cmd, "AI ", 3) == 0) {
        const char *arg = cmd + 3;

        if (strcmp(arg, "on") == 0) {
            baby_ai_set_enabled(true);
            snprintf(reply, sizeof(reply), "OK ai=on\n");
        } else if (strcmp(arg, "off") == 0) {
            baby_ai_set_enabled(false);
            snprintf(reply, sizeof(reply), "OK ai=off\n");
        } else if (strcmp(arg, "status") == 0) {
            baby_ai_status_t ai = baby_ai_get_status();
            const char *state_names[] = {
                "unknown", "absent", "awake", "sleeping", "alert"
            };
            const char *sname = (ai.state >= 0 && ai.state <= 4)
                                ? state_names[ai.state] : "unknown";
            snprintf(reply, sizeof(reply),
                     "{\"ai_enabled\":%s,\"baby_state\":\"%s\","
                     "\"confidence\":%.2f,\"motion\":%.2f,"
                     "\"face_visible\":%s}\n",
                     baby_ai_is_enabled() ? "true" : "false",
                     sname, ai.confidence, ai.motion_level,
                     ai.face_visible ? "true" : "false");
        } else {
            snprintf(reply, sizeof(reply), "ERR unknown AI arg: %s\n", arg);
        }

    } else if (strcmp(cmd, "STATUS") == 0) {
        pthread_mutex_lock(g_lock);
        /*
         * Reply with JSON so that server.py can json.loads() the response
         * directly.  Includes AI status for the web UI.
         */
        baby_ai_status_t ai = baby_ai_get_status();
        const char *state_names[] = {
            "unknown", "absent", "awake", "sleeping", "alert"
        };
        const char *ai_state = (ai.state >= 0 && ai.state <= 4)
                                ? state_names[ai.state] : "unknown";
        snprintf(reply, sizeof(reply),
                 "{\"mode\":\"%s\",\"source\":\"%s\",\"zoom\":%d,"
                 "\"ir\":\"%s\",\"ai\":%s,\"baby_state\":\"%s\","
                 "\"motion\":%.2f,\"fps\":30,\"camera\":true}\n",
                 layout_name(g_display->layout),
                 source_name(g_display->source),
                 (int)(g_display->cam_zoom * 100 + 0.5f),
                 ir_name(g_ir_state),
                 baby_ai_is_enabled() ? "true" : "false",
                 ai_state, ai.motion_level);
        pthread_mutex_unlock(g_lock);

    } else {
        snprintf(reply, sizeof(reply), "ERR unknown command: %s\n", cmd);
    }

send_reply:
    if (reply_fd >= 0 && reply[0] != '\0') {
        (void)send(reply_fd, reply, strlen(reply), MSG_NOSIGNAL);
    }
}

/*
 * Read data from a connected client into its line buffer, dispatching
 * complete lines as commands.
 * Returns 0 if the client is still alive, -1 if it should be removed.
 */
static int client_read(ctrl_client_t *c)
{
    char tmp[256];
    ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0; /* no data right now, not an error */
        return -1;    /* disconnect or error */
    }

    for (ssize_t i = 0; i < n; i++) {
        char ch = tmp[i];
        if (ch == '\r') continue; /* ignore CR in CRLF */

        if (ch == '\n') {
            /* Dispatch the accumulated line */
            c->line_buf[c->line_len] = '\0';
            if (c->line_len > 0)
                dispatch_command(c->line_buf, c->fd);
            c->line_len = 0;
        } else {
            if (c->line_len < LINE_BUF_SIZE - 1) {
                c->line_buf[c->line_len++] = ch;
            }
            /* If line overflows, silently drop tail — will result in parse error */
        }
    }
    return 0;
}

/* ---- Accept helpers ---- */

static void try_accept(int server_fd)
{
    while (1) {
        int cfd = accept(server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* no more pending connections */
            if (errno == EINTR) continue;
            perror("control: accept");
            break;
        }
        client_add(cfd);
    }
}

/* ---- Public API ---- */

int control_init(display_state_t *display, camera_t *cam,
                 pthread_mutex_t *lock, ir_control_cb ir_cb)
{
    g_display = display;
    g_cam     = cam;
    g_lock    = lock;
    g_ir_cb   = ir_cb;
    g_num_clients = 0;

    g_tcp_server  = create_tcp_server(CONTROL_TCP_PORT);
    g_unix_server = create_unix_server(CONTROL_SOCK_PATH);

    if (g_tcp_server < 0 && g_unix_server < 0) {
        fprintf(stderr, "control: failed to create any listener\n");
        return -1;
    }
    return 0;
}

void control_poll(void)
{
    if (!g_display || !g_lock)
        return;

    /* Accept new connections on both servers */
    if (g_tcp_server >= 0)  try_accept(g_tcp_server);
    if (g_unix_server >= 0) try_accept(g_unix_server);

    /* Service existing clients — iterate backwards so removals are safe */
    for (int i = g_num_clients - 1; i >= 0; i--) {
        if (client_read(&g_clients[i]) < 0)
            client_remove(i);
    }
}

void control_destroy(void)
{
    /* Close all clients */
    for (int i = 0; i < g_num_clients; i++)
        close(g_clients[i].fd);
    g_num_clients = 0;

    if (g_tcp_server >= 0)  { close(g_tcp_server);  g_tcp_server  = -1; }
    if (g_unix_server >= 0) { close(g_unix_server); g_unix_server = -1; }

    /* Close aa-proxy control connection */
    if (g_aa_ctrl_fd >= 0) { close(g_aa_ctrl_fd); g_aa_ctrl_fd = -1; }

    unlink(CONTROL_SOCK_PATH);
    printf("control: destroyed\n");
}
