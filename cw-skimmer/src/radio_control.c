#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifdef _WIN32
#include "win_compat.h"
#endif

#include "radio_control.h"
#include "tci_client.h"
#include "config.h"
#include "logger.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>
#endif

#define RC_QUEUE 32
#define RC_TEXT 360
#define RC_BACKLOG 1024
#define KY_CHUNK 24

enum {
    RC_OP_TUNE = 1,
    RC_OP_CW = 2,
    RC_OP_STOP = 3
};

enum {
    KY_DOWN = 0,
    KY_CONNECTING = 1,
    KY_READY = 2
};

typedef struct {
    int kind;
    long long hz;
    char text[RC_TEXT];
} rc_op_t;

struct radio_control {
    pthread_mutex_t lock;
    rc_op_t queue[RC_QUEUE];
    int q_head;
    int q_count;

    char mode[8];
    char cat_host[256];
    int cat_port;
    int wpm;
    int speed_dirty;
    int ky_warned;
    int fallback_logged;

    int ky_fd;
    int ky_state;
    char ky_rx[512];
    int ky_rx_len;
    int ky_awaiting;
    long long ky_next_ms;
    char ky_backlog[RC_BACKLOG];
    int ky_backlog_len;
    int ky_pos;
};

static void rc_ky_send_speed(radio_control_t *rc);

static long long rc_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static int rc_clamp_wpm(int wpm)
{
    if (wpm < 5) {
        return 5;
    }
    if (wpm > 60) {
        return 60;
    }
    return wpm;
}

int radio_control_sanitize_cw(const char *in, char *out, int out_max)
{
    int j = 0;

    if (!out || out_max < 2) {
        return -1;
    }
    out[0] = '\0';
    if (!in) {
        return 0;
    }

    for (const unsigned char *p = (const unsigned char *)in; *p && j < out_max - 1; ++p) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 'a' + 'A');
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '.' || c == ',' || c == '?' || c == '/' ||
            c == '-' || c == '=' || c == '+') {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return j;
}

int radio_control_format_tune(int rx, long long hz,
                              char *dds, int dds_max,
                              char *vfo, int vfo_max)
{
    if (rx < 0) {
        rx = 0;
    }
    if (hz < 100000LL || hz > 6000000000LL) {
        return -1;
    }
    if (!dds || !vfo || dds_max < 32 || vfo_max < 40) {
        return -1;
    }
    snprintf(dds, (size_t)dds_max, "dds:%d,%lld;", rx, hz);
    snprintf(vfo, (size_t)vfo_max, "vfo:%d,0,%lld;", rx, hz);
    return 0;
}

int radio_control_format_cw_macro(int rx, const char *text, char *cmd, int cmd_max)
{
    char clean[RC_TEXT];
    int n;

    if (!cmd || cmd_max < 16) {
        return -1;
    }
    cmd[0] = '\0';
    if (rx < 0) {
        rx = 0;
    }
    n = radio_control_sanitize_cw(text, clean, (int)sizeof(clean));
    if (n <= 0) {
        return 0;
    }
    if (snprintf(cmd, (size_t)cmd_max, "cw_macros:%d,%s;", rx, clean) >= cmd_max) {
        return -1;
    }
    return n;
}

int radio_control_format_wpm(int wpm, char *keyer, int keyer_max,
                             char *macros, int macros_max)
{
    wpm = rc_clamp_wpm(wpm);
    if (!keyer || !macros || keyer_max < 24 || macros_max < 28) {
        return -1;
    }
    snprintf(keyer, (size_t)keyer_max, "cw_keyer_speed:%d;", wpm);
    snprintf(macros, (size_t)macros_max, "cw_macros_speed:%d;", wpm);
    return wpm;
}

int radio_control_ky_chunk(const char *text, int *pos, char *cmd, int cmd_max)
{
    int start;
    int n;

    if (!text || !pos || !cmd || cmd_max < 8) {
        return 0;
    }
    start = *pos;
    if (start < 0) {
        start = 0;
    }
    if (text[start] == '\0') {
        return 0;
    }
    n = 0;
    while (text[start + n] != '\0' && n < KY_CHUNK) {
        n++;
    }
    if (n <= 0 || n + 5 > cmd_max) {
        return 0;
    }
    cmd[0] = 'K';
    cmd[1] = 'Y';
    cmd[2] = ' ';
    memcpy(cmd + 3, text + start, (size_t)n);
    cmd[3 + n] = ';';
    cmd[4 + n] = '\0';
    *pos = start + n;
    return 1;
}

int radio_control_ky_status(const char *line)
{
    const char *p;

    if (!line) {
        return -1;
    }
    p = strstr(line, "KY");
    if (!p) {
        return -1;
    }
    p += 2;
    while (*p == ' ') {
        p++;
    }
    if (*p == '0') {
        return 0;
    }
    if (*p == '1') {
        return 1;
    }
    return -1;
}

static void rc_ky_close(radio_control_t *rc)
{
    if (rc->ky_fd >= 0) {
#ifdef _WIN32
        closesocket((SOCKET)rc->ky_fd);
#else
        close(rc->ky_fd);
#endif
        rc->ky_fd = -1;
    }
    rc->ky_state = KY_DOWN;
    rc->ky_rx_len = 0;
    rc->ky_awaiting = 0;
    rc->ky_rx[0] = '\0';
}

static void rc_clear_queue_locked(radio_control_t *rc)
{
    rc->q_head = 0;
    rc->q_count = 0;
    rc->ky_backlog_len = 0;
    rc->ky_backlog[0] = '\0';
    rc->ky_pos = 0;
}

static int rc_push_locked(radio_control_t *rc, const rc_op_t *op)
{
    int idx;

    if (rc->q_count >= RC_QUEUE) {
        rc->q_head = (rc->q_head + 1) % RC_QUEUE;
        rc->q_count--;
    }
    idx = (rc->q_head + rc->q_count) % RC_QUEUE;
    rc->queue[idx] = *op;
    rc->q_count++;
    return 0;
}

static void rc_drop_kind_locked(radio_control_t *rc, int kind)
{
    rc_op_t kept[RC_QUEUE];
    int n = 0;
    int i;

    for (i = 0; i < rc->q_count; i++) {
        rc_op_t *op = &rc->queue[(rc->q_head + i) % RC_QUEUE];
        if (op->kind != kind) {
            kept[n++] = *op;
        }
    }
    rc->q_head = 0;
    rc->q_count = n;
    for (i = 0; i < n; i++) {
        rc->queue[i] = kept[i];
    }
}

radio_control_t *radio_control_create(void)
{
    radio_control_t *rc = calloc(1, sizeof(*rc));
    if (!rc) {
        return NULL;
    }
    pthread_mutex_init(&rc->lock, NULL);
    strcpy(rc->mode, "tci");
    strcpy(rc->cat_host, "127.0.0.1");
    rc->wpm = 20;
    rc->ky_fd = -1;
    rc->speed_dirty = 1;
    return rc;
}

void radio_control_destroy(radio_control_t *rc)
{
    if (!rc) {
        return;
    }
    rc_ky_close(rc);
    pthread_mutex_destroy(&rc->lock);
    free(rc);
}

void radio_control_apply_config(radio_control_t *rc, const config_t *cfg)
{
    char host[256];
    int port;
    int ky;
    int host_changed;

    if (!rc || !cfg) {
        return;
    }

    strncpy(host, cfg->cat_host[0] ? cfg->cat_host : "127.0.0.1", sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    port = cfg->cat_port;
    ky = (strcasecmp(cfg->cw_send, "ky") == 0 && port > 0);

    pthread_mutex_lock(&rc->lock);
    host_changed = (strcmp(rc->cat_host, host) != 0 || rc->cat_port != port);
    strncpy(rc->cat_host, host, sizeof(rc->cat_host) - 1);
    rc->cat_host[sizeof(rc->cat_host) - 1] = '\0';
    rc->cat_port = port;
    if (ky) {
        strcpy(rc->mode, "ky");
    } else {
        strcpy(rc->mode, "tci");
        if (strcasecmp(cfg->cw_send, "ky") == 0 && port <= 0 && !rc->fallback_logged) {
            rc->fallback_logged = 1;
            pthread_mutex_unlock(&rc->lock);
            LOG_INFO("cw_send=ky needs cat_port. CW text stays on TCI cw_macros.");
            pthread_mutex_lock(&rc->lock);
        }
    }
    rc->wpm = rc_clamp_wpm(cfg->cw_wpm > 0 ? cfg->cw_wpm : 20);
    rc->speed_dirty = 1;
    if (host_changed || !ky) {
        rc_ky_close(rc);
    }
    pthread_mutex_unlock(&rc->lock);
}

const char *radio_control_send_mode(const radio_control_t *rc)
{
    if (!rc) {
        return "tci";
    }
    return rc->mode;
}

int radio_control_tune(radio_control_t *rc, long long hz)
{
    rc_op_t op;

    if (!rc) {
        return -1;
    }
    if (hz < 100000LL || hz > 6000000000LL) {
        return -1;
    }
    memset(&op, 0, sizeof(op));
    op.kind = RC_OP_TUNE;
    op.hz = hz;
    pthread_mutex_lock(&rc->lock);
    rc_drop_kind_locked(rc, RC_OP_TUNE);
    rc_push_locked(rc, &op);
    pthread_mutex_unlock(&rc->lock);
    return 0;
}

int radio_control_cw_text(radio_control_t *rc, const char *text)
{
    rc_op_t op;
    char clean[RC_TEXT];
    int n;

    if (!rc) {
        return -1;
    }
    n = radio_control_sanitize_cw(text, clean, (int)sizeof(clean));
    if (n <= 0) {
        return 0;
    }
    memset(&op, 0, sizeof(op));
    op.kind = RC_OP_CW;
    memcpy(op.text, clean, (size_t)n + 1);
    pthread_mutex_lock(&rc->lock);
    rc_push_locked(rc, &op);
    pthread_mutex_unlock(&rc->lock);
    return 0;
}

int radio_control_cw_stop(radio_control_t *rc)
{
    rc_op_t op;

    if (!rc) {
        return -1;
    }
    memset(&op, 0, sizeof(op));
    op.kind = RC_OP_STOP;
    pthread_mutex_lock(&rc->lock);
    rc_drop_kind_locked(rc, RC_OP_CW);
    rc->ky_backlog_len = 0;
    rc->ky_backlog[0] = '\0';
    rc->ky_pos = 0;
    rc_push_locked(rc, &op);
    pthread_mutex_unlock(&rc->lock);
    return 0;
}

int radio_control_set_wpm(radio_control_t *rc, int wpm)
{
    if (!rc) {
        return -1;
    }
    pthread_mutex_lock(&rc->lock);
    rc->wpm = rc_clamp_wpm(wpm);
    rc->speed_dirty = 1;
    pthread_mutex_unlock(&rc->lock);
    return 0;
}

static int rc_send_tci(tci_client_t *tci, const char *cmd)
{
    if (!tci || !cmd || !cmd[0]) {
        return -1;
    }
    if (tci_send_command(tci, cmd) != 0) {
        LOG_WARN("Radio command failed: %s", cmd);
        return -1;
    }
    return 0;
}

static int rc_ky_write(radio_control_t *rc, const char *cmd)
{
    size_t len;
    ssize_t n;

    if (!rc || rc->ky_fd < 0 || !cmd) {
        return -1;
    }
    len = strlen(cmd);
#ifdef _WIN32
    n = send((SOCKET)rc->ky_fd, cmd, (int)len, MSG_NOSIGNAL);
#else
    n = send(rc->ky_fd, cmd, len, MSG_NOSIGNAL);
#endif
    if (n < 0 || (size_t)n != len) {
        LOG_WARN("KY send failed: %s", cmd);
        rc_ky_close(rc);
        return -1;
    }
    return 0;
}

static int rc_ky_open(radio_control_t *rc)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char port[16];
    int fd;
    int rc_connect;
    int flags;

    if (rc->cat_port <= 0 || rc->cat_host[0] == '\0') {
        return -1;
    }
    if (rc_now_ms() < rc->ky_next_ms) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", rc->cat_port);
    if (getaddrinfo(rc->cat_host, port, &hints, &res) != 0 || !res) {
        rc->ky_next_ms = rc_now_ms() + 2000;
        if (!rc->ky_warned) {
            rc->ky_warned = 1;
            LOG_WARN("KY CAT lookup failed for %s:%d", rc->cat_host, rc->cat_port);
        }
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        rc->ky_next_ms = rc_now_ms() + 2000;
        return -1;
    }
#ifdef _WIN32
    {
        u_long on = 1;
        (void)flags;
        ioctlsocket((SOCKET)fd, FIONBIO, &on);
    }
#else
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    rc_connect = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
#ifdef _WIN32
    if (rc_connect < 0) {
        int werr = WSAGetLastError();
        if (werr != WSAEWOULDBLOCK && werr != WSAEINPROGRESS) {
            closesocket((SOCKET)fd);
            rc->ky_next_ms = rc_now_ms() + 2000;
            if (!rc->ky_warned) {
                rc->ky_warned = 1;
                LOG_WARN("KY CAT connect failed for %s:%d", rc->cat_host, rc->cat_port);
            }
            return -1;
        }
    }
#else
    if (rc_connect < 0 && errno != EINPROGRESS) {
        close(fd);
        rc->ky_next_ms = rc_now_ms() + 2000;
        if (!rc->ky_warned) {
            rc->ky_warned = 1;
            LOG_WARN("KY CAT connect failed for %s:%d", rc->cat_host, rc->cat_port);
        }
        return -1;
    }
#endif
    rc->ky_fd = fd;
    rc->ky_state = (rc_connect == 0) ? KY_READY : KY_CONNECTING;
    rc->ky_rx_len = 0;
    rc->ky_awaiting = 0;
    if (rc->ky_state == KY_READY) {
        LOG_INFO("KY CAT connected to %s:%d", rc->cat_host, rc->cat_port);
        rc->ky_warned = 0;
        rc_ky_send_speed(rc);
    }
    return 0;
}

static void rc_ky_send_speed(radio_control_t *rc)
{
    char ks[16];

    if (rc->ky_state != KY_READY) {
        return;
    }
    snprintf(ks, sizeof(ks), "KS%03d;", rc_clamp_wpm(rc->wpm));
    rc_ky_write(rc, ks);
}

static void rc_ky_finish_connect(radio_control_t *rc)
{
    int err = 0;
#ifdef _WIN32
    WSAPOLLFD pfd;
    int len = (int)sizeof(err);

    pfd.fd = (SOCKET)rc->ky_fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (WSAPoll(&pfd, 1, 0) <= 0) {
        return;
    }
    if (getsockopt((SOCKET)rc->ky_fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 || err != 0) {
#else
    struct pollfd pfd;
    socklen_t len = sizeof(err);

    pfd.fd = rc->ky_fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0) {
        return;
    }
    if (getsockopt(rc->ky_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
#endif
        LOG_WARN("KY CAT connect failed for %s:%d", rc->cat_host, rc->cat_port);
        rc_ky_close(rc);
        rc->ky_next_ms = rc_now_ms() + 2000;
        return;
    }
    rc->ky_state = KY_READY;
    LOG_INFO("KY CAT connected to %s:%d", rc->cat_host, rc->cat_port);
    rc->ky_warned = 0;
    rc_ky_send_speed(rc);
}

static void rc_ky_read(radio_control_t *rc)
{
    unsigned char buf[256];
    ssize_t n;

    if (rc->ky_fd < 0) {
        return;
    }
#ifdef _WIN32
    n = recv((SOCKET)rc->ky_fd, (char *)buf, (int)sizeof(buf), 0);
#else
    n = recv(rc->ky_fd, buf, sizeof(buf), 0);
#endif
    if (n == 0) {
        rc_ky_close(rc);
        return;
    }
    if (n < 0) {
#ifdef _WIN32
        int werr = WSAGetLastError();
        if (werr == WSAEWOULDBLOCK || werr == WSAEINPROGRESS) {
            return;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
#endif
        rc_ky_close(rc);
        return;
    }
    if (rc->ky_rx_len + (int)n >= (int)sizeof(rc->ky_rx)) {
        rc->ky_rx_len = 0;
    }
    memcpy(rc->ky_rx + rc->ky_rx_len, buf, (size_t)n);
    rc->ky_rx_len += (int)n;
    rc->ky_rx[rc->ky_rx_len] = '\0';
}

static void rc_ky_take_line(radio_control_t *rc, char *line, int line_max)
{
    char *semi;
    int keep;

    line[0] = '\0';
    semi = strchr(rc->ky_rx, ';');
    if (!semi) {
        return;
    }
    keep = (int)(semi - rc->ky_rx);
    if (keep >= line_max) {
        keep = line_max - 1;
    }
    memcpy(line, rc->ky_rx, (size_t)keep);
    line[keep] = '\0';
    keep = (int)(semi - rc->ky_rx) + 1;
    memmove(rc->ky_rx, rc->ky_rx + keep, (size_t)(rc->ky_rx_len - keep));
    rc->ky_rx_len -= keep;
    rc->ky_rx[rc->ky_rx_len] = '\0';
}

static void rc_ky_append(radio_control_t *rc, const char *text)
{
    int n;
    int room;

    if (!text || !text[0]) {
        return;
    }
    n = (int)strlen(text);
    room = RC_BACKLOG - 1 - rc->ky_backlog_len;
    if (n > room) {
        n = room;
        LOG_WARN("KY backlog full, CW text truncated");
    }
    if (n <= 0) {
        return;
    }
    memcpy(rc->ky_backlog + rc->ky_backlog_len, text, (size_t)n);
    rc->ky_backlog_len += n;
    rc->ky_backlog[rc->ky_backlog_len] = '\0';
}

static void rc_ky_service(radio_control_t *rc)
{
    char line[256];
    char cmd[40];
    int status;

    if (strcmp(rc->mode, "ky") != 0) {
        return;
    }
    if (rc->ky_state == KY_DOWN) {
        rc_ky_open(rc);
    }
    if (rc->ky_state == KY_CONNECTING) {
        rc_ky_finish_connect(rc);
    }
    if (rc->ky_state != KY_READY) {
        return;
    }

    rc_ky_read(rc);
    while (rc->ky_state == KY_READY) {
        rc_ky_take_line(rc, line, (int)sizeof(line));
        if (!line[0]) {
            break;
        }
        status = radio_control_ky_status(line);
        if (status < 0) {
            continue;
        }
        rc->ky_awaiting = 0;
        if (status == 1) {
            rc->ky_next_ms = rc_now_ms() + 40;
            continue;
        }
        if (rc->ky_pos < rc->ky_backlog_len &&
            radio_control_ky_chunk(rc->ky_backlog, &rc->ky_pos, cmd, (int)sizeof(cmd))) {
            if (rc_ky_write(rc, cmd) != 0) {
                return;
            }
            LOG_DEBUG("KY sent: %s", cmd);
        }
        if (rc->ky_pos >= rc->ky_backlog_len) {
            rc->ky_backlog_len = 0;
            rc->ky_backlog[0] = '\0';
            rc->ky_pos = 0;
        }
    }

    if (rc->ky_state == KY_READY && !rc->ky_awaiting &&
        rc_now_ms() >= rc->ky_next_ms &&
        rc->ky_pos < rc->ky_backlog_len) {
        if (rc_ky_write(rc, "KY;") == 0) {
            rc->ky_awaiting = 1;
        }
    }
}

static void rc_send_speed(radio_control_t *rc, tci_client_t *tci, int wpm)
{
    char keyer[32];
    char macros[32];

    if (radio_control_format_wpm(wpm, keyer, (int)sizeof(keyer),
                                 macros, (int)sizeof(macros)) < 0) {
        return;
    }
    rc_send_tci(tci, keyer);
    rc_send_tci(tci, macros);
    rc_ky_send_speed(rc);
}

void radio_control_service(radio_control_t *rc, tci_client_t *tci)
{
    rc_op_t batch[RC_QUEUE];
    int n = 0;
    int i;
    int send_wpm = 0;
    int wpm = 20;
    int rx;
    char mode[8];

    if (!rc) {
        return;
    }

    if (tci_training_active()) {
        pthread_mutex_lock(&rc->lock);
        rc_clear_queue_locked(rc);
        pthread_mutex_unlock(&rc->lock);
        return;
    }

    pthread_mutex_lock(&rc->lock);
    memcpy(mode, rc->mode, sizeof(mode));
    if (tci && tci_is_connected(tci)) {
        if (rc->speed_dirty) {
            send_wpm = 1;
            wpm = rc->wpm;
            rc->speed_dirty = 0;
        }
        while (n < RC_QUEUE && rc->q_count > 0) {
            batch[n++] = rc->queue[rc->q_head];
            rc->q_head = (rc->q_head + 1) % RC_QUEUE;
            rc->q_count--;
        }
    }
    pthread_mutex_unlock(&rc->lock);

    if (!tci || !tci_is_connected(tci)) {
        return;
    }

    rx = tci->iq_vfo >= 0 ? tci->iq_vfo : 0;
    if (strcmp(mode, "ky") == 0) {
        pthread_mutex_lock(&rc->lock);
        rc_ky_service(rc);
        pthread_mutex_unlock(&rc->lock);
    }
    if (send_wpm) {
        pthread_mutex_lock(&rc->lock);
        rc_send_speed(rc, tci, wpm);
        pthread_mutex_unlock(&rc->lock);
    }

    for (i = 0; i < n; i++) {
        if (batch[i].kind == RC_OP_TUNE) {
            char dds[64];
            char vfo[64];
            if (radio_control_format_tune(rx, batch[i].hz, dds, (int)sizeof(dds),
                                          vfo, (int)sizeof(vfo)) == 0) {
                LOG_INFO("Tune radio to %lld Hz", batch[i].hz);
                rc_send_tci(tci, dds);
                rc_send_tci(tci, vfo);
            }
        } else if (batch[i].kind == RC_OP_STOP) {
            rc_send_tci(tci, "cw_macros_stop;");
            LOG_INFO("CW send stopped");
        } else if (batch[i].kind == RC_OP_CW) {
            if (strcmp(mode, "ky") == 0) {
                pthread_mutex_lock(&rc->lock);
                rc_ky_append(rc, batch[i].text);
                rc_ky_service(rc);
                pthread_mutex_unlock(&rc->lock);
            } else {
                char cmd[RC_TEXT + 32];
                if (radio_control_format_cw_macro(rx, batch[i].text, cmd, (int)sizeof(cmd)) > 0) {
                    LOG_DEBUG("CW text: %s", cmd);
                    rc_send_tci(tci, cmd);
                }
            }
        }
    }
}
