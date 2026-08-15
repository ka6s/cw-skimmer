#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "tci_client.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <libwebsockets.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

/* usleep declaration for strict C99 (-std=c99) builds where unistd may hide it without _BSD_SOURCE */
#if !defined(_BSD_SOURCE) && !defined(_DEFAULT_SOURCE) && !defined(_XOPEN_SOURCE)
int usleep(unsigned int __useconds);
#endif

#define IQ_BUFFER_SIZE 480000
#define TCI_TCP_CONNECT_TIMEOUT_MS 5000
#define TCI_RX_BUFFER_SIZE 65536
#define TCI_STREAM_HEADER_SIZE 64
#define TCI_STREAM_IQ 0
#define TCI_STREAM_RX_AUDIO 1
#define TCI_AUDIO_FORMAT_FLOAT32 3
#define TCI_RX_AUDIO_FRAME_FRAMES 512
#define TCI_AUDIO_CHANNELS 2
#define TCI_AUDIO_RX_FRAME_MAX_BYTES \
    (TCI_STREAM_HEADER_SIZE + (TCI_RX_AUDIO_FRAME_FRAMES * TCI_AUDIO_CHANNELS * sizeof(float)))

/* Verbose TCI connection spam — enable with -DCONN_DEBUG when debugging connect */
#ifdef CONN_DEBUG
#define CONN_DBG(fmt, ...) do { \
    fprintf(stderr, "[CONN] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)
#else
#define CONN_DBG(fmt, ...) ((void)0)
#endif

typedef struct {
    uint32_t receiver;
    uint32_t sample_rate;
    uint32_t format;
    uint32_t codec;
    uint32_t crc;
    uint32_t length;
    uint32_t type;
    uint32_t channels;
    uint32_t reserv[8];
} tci_stream_header_t;

typedef struct {
    tci_client_t *client;
    struct lws_context *context;
    struct lws *wsi;
    pthread_mutex_t lock;
    int connected;
    int connect_failed;
    unsigned char *rx_buffer;
    size_t rx_buffer_len;
} tci_ws_context_t;

typedef struct {
    tci_client_t *client;
    unsigned char *stream_buffer;
    size_t stream_len;
    size_t stream_size;
    unsigned char *audio_buffer;
    size_t audio_len;
    size_t audio_size;
    pthread_mutex_t lock;
} tci_tcp_context_t;

static tci_ws_context_t *g_ws_context = NULL;
static tci_tcp_context_t *g_tcp_context = NULL;

static int tci_tcp_connect_socket(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

static tci_transport_t tci_parse_transport(const char *protocol) {
    if (!protocol) {
        return TCI_TRANSPORT_TCP;
    }
    if (strcasecmp(protocol, "websocket") == 0 || strcasecmp(protocol, "ws") == 0) {
        return TCI_TRANSPORT_WEBSOCKET;
    }
    return TCI_TRANSPORT_TCP;
}

static const char *tci_transport_name(tci_transport_t transport) {
    return transport == TCI_TRANSPORT_WEBSOCKET ? "WebSocket" : "TCP/IP";
}

static int tci_is_text_start(unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9');
}

/* 1 after successful audio_start; 0 when using wideband iq_start */
static int g_stream_audio_only = 0;
/* User / GUI preference (default: complex IQ) */
static tci_stream_mode_t g_stream_mode_pref = TCI_STREAM_MODE_IQ;

void tci_set_stream_mode_preference(tci_stream_mode_t mode)
{
    if (mode != TCI_STREAM_MODE_AUDIO) {
        mode = TCI_STREAM_MODE_IQ;
    }
    g_stream_mode_pref = mode;
    LOG_INFO("TCI stream preference → %s",
             mode == TCI_STREAM_MODE_AUDIO ? "AUDIO (demodulated)" : "IQ (complex baseband)");
}

tci_stream_mode_t tci_get_stream_mode_preference(void)
{
    return g_stream_mode_pref;
}

void tci_clear_sample_buffer(tci_client_t *client)
{
    if (!client) {
        return;
    }
    client->buffer_head = 0;
    client->buffer_count = 0;
}

static int tci_uses_wideband_iq(const tci_client_t *client) {
    /* Prefer wideband iq_start when the server supports it (deskHPSDR, Thetis, ExpertSDR). */
    (void)client;
    return 1;
}

static void tci_parse_text_response(tci_client_t *client, const char *text) {
    if (!client || !text) {
        return;
    }

    if (strstr(text, "protocol:") == text) {
        char proto[sizeof(client->remote_protocol)] = {0};
        if (sscanf(text, "protocol:%63[^,];", proto) == 1) {
            strncpy(client->remote_protocol, proto, sizeof(client->remote_protocol) - 1);
            LOG_INFO("Remote TCI protocol: %s", client->remote_protocol);
            CONN_DBG("Remote TCI protocol: %s", client->remote_protocol);
        }
    } else if (strstr(text, "device:") == text) {
        char device[sizeof(client->remote_device)] = {0};
        if (sscanf(text, "device:%63[^;];", device) == 1) {
            strncpy(client->remote_device, device, sizeof(client->remote_device) - 1);
            LOG_INFO("Remote TCI device: %s", client->remote_device);
            CONN_DBG("Remote TCI device: %s", client->remote_device);
        }
    } else if (strstr(text, "vfo:") == text) {
        /* Formats: vfo:0,0,14074000;  or  vfo:0,0,14074000.0  (semicolon optional)
         * Only channel 0 (main RX VFO) updates the skimmer center — ch1 is the
         * secondary VFO and must not overwrite (deskHPSDR sends both at connect). */
        int vfo_idx = -1;
        int ch = -1;
        double freq_d = 0.0;
        long long freq = 0;
        if (sscanf(text, "vfo:%d,%d,%lf", &vfo_idx, &ch, &freq_d) >= 3 && freq_d > 1.0e5) {
            freq = (long long)(freq_d + 0.5);
            if (vfo_idx == client->iq_vfo && ch == 0) {
                client->center_frequency = freq;
                LOG_INFO("VFO %d ch%d frequency: %lld Hz", vfo_idx, ch, freq);
                CONN_DBG("VFO %d ch%d: %lld Hz (skimmer center)", vfo_idx, ch, freq);
            } else {
                CONN_DBG("VFO %d ch%d: %lld Hz (ignored for center)", vfo_idx, ch, freq);
            }
        }
    } else if (strstr(text, "dds:") == text) {
        /* Formats: dds:0,14074000;  or  dds:0,14074000.0 — IQ stream LO / tune center */
        int dds_idx = -1;
        double freq_d = 0.0;
        long long freq = 0;
        if (sscanf(text, "dds:%d,%lf", &dds_idx, &freq_d) >= 2 && freq_d > 1.0e5) {
            freq = (long long)(freq_d + 0.5);
            if (dds_idx == client->iq_vfo) {
                client->center_frequency = freq;
                LOG_INFO("DDS %d frequency: %lld Hz (IQ center)", dds_idx, freq);
                CONN_DBG("DDS %d: %lld Hz (skimmer center)", dds_idx, freq);
            } else {
                CONN_DBG("DDS %d: %lld Hz (ignored, stream on %d)",
                         dds_idx, freq, client->iq_vfo);
            }
        }
    } else if (strstr(text, "tx_frequency:") == text) {
        double freq_d = 0.0;
        if (sscanf(text, "tx_frequency:%lf", &freq_d) == 1 && freq_d > 1.0e5) {
            /* TX freq is informational only — do not replace the RX/I/Q center. */
            CONN_DBG("TX frequency: %.0f Hz (not used for I/Q center)", freq_d);
        }
    } else {
        CONN_DBG("TCI text: %s", text);
    }
}

static int tci_stage_audio_nolock(tci_client_t *client, const unsigned char *data, size_t len) {
    unsigned char *dst = NULL;
    size_t *dst_len = NULL;
    size_t *dst_size = NULL;

    if (client->transport == TCI_TRANSPORT_WEBSOCKET) {
        if (!g_ws_context) {
            return -1;
        }
        dst = g_ws_context->rx_buffer;
        dst_len = &g_ws_context->rx_buffer_len;
    } else {
        if (!g_tcp_context) {
            return -1;
        }
        dst = g_tcp_context->audio_buffer;
        dst_len = &g_tcp_context->audio_len;
        dst_size = &g_tcp_context->audio_size;
    }

    if (client->transport == TCI_TRANSPORT_TCP && dst_size) {
        size_t needed = *dst_len + len;
        if (needed > *dst_size) {
            size_t new_size = *dst_size ? *dst_size : 8192;
            while (new_size < needed) {
                new_size *= 2;
            }
            unsigned char *grown = realloc(dst, new_size);
            if (!grown) {
                return -1;
            }
            g_tcp_context->audio_buffer = grown;
            dst = grown;
            *dst_size = new_size;
        }
    } else {
        size_t needed = *dst_len + len;
        if (needed > TCI_RX_BUFFER_SIZE) {
            LOG_WARN("Audio staging overflow: %zu bytes", needed);
            *dst_len = 0;
            return -1;
        }
    }

    memcpy(dst + *dst_len, data, len);
    *dst_len += len;
    return 0;
}

static int tci_is_iq_stream_type(uint32_t type) {
    return type == TCI_STREAM_IQ || type == TCI_STREAM_RX_AUDIO;
}

/* Consume as many complete TCI stream frames as possible from buffer.
 * Returns samples written; *consumed is bytes eaten from the front of buffer. */
static int tci_process_audio_payload(tci_client_t *client, const unsigned char *buffer,
                                     size_t bytes_to_process, size_t *consumed) {
    int processed = 0;
    size_t offset = 0;

    if (consumed) {
        *consumed = 0;
    }
    if (!client || !buffer || bytes_to_process < sizeof(float) * TCI_AUDIO_CHANNELS) {
        return 0;
    }

    while (offset + TCI_STREAM_HEADER_SIZE <= bytes_to_process) {
        const tci_stream_header_t *header =
            (const tci_stream_header_t *)(buffer + offset);
        size_t payload_bytes;
        size_t frame_bytes;
        const float *audio_data;
        int num_floats;
        int num_iq_pairs;
        int i;

        if (!tci_is_iq_stream_type(header->type) ||
            header->format != TCI_AUDIO_FORMAT_FLOAT32 ||
            header->length == 0 ||
            header->length > 65536) {
            break; /* not a framed stream — fall back below */
        }

        payload_bytes = (size_t)header->length * sizeof(float);
        frame_bytes = TCI_STREAM_HEADER_SIZE + payload_bytes;
        if (offset + frame_bytes > bytes_to_process) {
            break; /* incomplete frame — keep for next read */
        }

        audio_data = (const float *)(buffer + offset + TCI_STREAM_HEADER_SIZE);
        num_floats = (int)(payload_bytes / sizeof(float));
        num_iq_pairs = num_floats / TCI_AUDIO_CHANNELS;

        /*
         * type 0 = true complex IQ (Thetis / ExpertSDR iq_start)
         * type 1 = stereo RX *audio* (deskHPSDR audio_start) — L/R are NOT I/Q.
         * Treating L/R as I/Q yields a strong center streak + mirror images.
         */
        if (header->type == TCI_STREAM_RX_AUDIO) {
            g_stream_audio_only = 1;
            for (i = 0; i < num_iq_pairs; i++) {
                float left = audio_data[i * 2];
                float right = audio_data[i * 2 + 1];
                float mono;
                if (!isfinite(left)) {
                    left = 0.0f;
                }
                if (!isfinite(right)) {
                    right = 0.0f;
                }
                mono = 0.5f * (left + right);
                client->iq_buffer[client->buffer_head] = mono + 0.0f * I;
                client->buffer_head = (client->buffer_head + 1) % client->buffer_size;
                if (client->buffer_count < client->buffer_size) {
                    client->buffer_count++;
                }
                processed++;
            }
        } else {
            g_stream_audio_only = 0;
            for (i = 0; i < num_iq_pairs; i++) {
                float i_val = audio_data[i * 2];
                float q_val = audio_data[i * 2 + 1];
                if (isfinite(i_val) && isfinite(q_val)) {
                    client->iq_buffer[client->buffer_head] = i_val + q_val * I;
                    client->buffer_head = (client->buffer_head + 1) % client->buffer_size;
                    if (client->buffer_count < client->buffer_size) {
                        client->buffer_count++;
                    }
                    processed++;
                }
            }
        }
        offset += frame_bytes;
    }

    if (processed > 0) {
        if (consumed) {
            *consumed = offset;
        }
        {
            static int bin_log = 0;
            if (++bin_log <= 5 || (bin_log % 100) == 0) {
                CONN_DBG("Binary stream: +%d samples (frames consumed %zu bytes, left %zu)",
                         processed, offset, bytes_to_process - offset);
            }
        }
        return processed;
    }

    /* No framed frames parsed — if buffer looks like raw stereo float32, take it */
    if (offset == 0 && bytes_to_process >= sizeof(float) * TCI_AUDIO_CHANNELS) {
        const float *audio_data = (const float *)buffer;
        int num_iq_pairs = (int)(bytes_to_process / (sizeof(float) * TCI_AUDIO_CHANNELS));
        int i;
        for (i = 0; i < num_iq_pairs; i++) {
            float i_val = audio_data[i * 2];
            float q_val = audio_data[i * 2 + 1];
            if (isfinite(i_val) && isfinite(q_val)) {
                client->iq_buffer[client->buffer_head] = i_val + q_val * I;
                client->buffer_head = (client->buffer_head + 1) % client->buffer_size;
                if (client->buffer_count < client->buffer_size) {
                    client->buffer_count++;
                }
                processed++;
            }
        }
        if (consumed) {
            *consumed = (size_t)num_iq_pairs * sizeof(float) * TCI_AUDIO_CHANNELS;
        }
    } else if (consumed) {
        *consumed = 0; /* keep incomplete framed data */
    }

    return processed;
}

static size_t tci_tcp_binary_frame_size(const unsigned char *data, size_t available) {
    const tci_stream_header_t *header;

    if (available < TCI_STREAM_HEADER_SIZE) {
        return 0;
    }

    header = (const tci_stream_header_t *)data;
    if (tci_is_iq_stream_type(header->type) && header->length > 0) {
        size_t frame_bytes = TCI_STREAM_HEADER_SIZE + (size_t)header->length * sizeof(float);
        if (frame_bytes <= TCI_AUDIO_RX_FRAME_MAX_BYTES + 1024) {
            return frame_bytes;
        }
    }

    if (tci_is_iq_stream_type(header->type)) {
        return TCI_AUDIO_RX_FRAME_MAX_BYTES;
    }

    return 0;
}

static void tci_tcp_consume_stream(size_t count) {
    if (!g_tcp_context || count == 0) {
        return;
    }

    if (count >= g_tcp_context->stream_len) {
        g_tcp_context->stream_len = 0;
        return;
    }

    memmove(g_tcp_context->stream_buffer,
            g_tcp_context->stream_buffer + count,
            g_tcp_context->stream_len - count);
    g_tcp_context->stream_len -= count;
}

static int tci_tcp_append_stream(const unsigned char *data, size_t len) {
    size_t needed;

    if (!g_tcp_context || !data || len == 0) {
        return -1;
    }

    needed = g_tcp_context->stream_len + len;
    if (needed > g_tcp_context->stream_size) {
        size_t new_size = g_tcp_context->stream_size ? g_tcp_context->stream_size : 8192;
        while (new_size < needed) {
            new_size *= 2;
        }
        unsigned char *grown = realloc(g_tcp_context->stream_buffer, new_size);
        if (!grown) {
            return -1;
        }
        g_tcp_context->stream_buffer = grown;
        g_tcp_context->stream_size = new_size;
    }

    memcpy(g_tcp_context->stream_buffer + g_tcp_context->stream_len, data, len);
    g_tcp_context->stream_len += len;
    return 0;
}

static void tci_tcp_parse_stream_buffer(tci_client_t *client) {
    if (!g_tcp_context) {
        return;
    }

    pthread_mutex_lock(&g_tcp_context->lock);
    while (g_tcp_context->stream_len > 0) {
        unsigned char first = g_tcp_context->stream_buffer[0];

        if (tci_is_text_start(first)) {
            unsigned char *semi = memchr(g_tcp_context->stream_buffer, ';', g_tcp_context->stream_len);
            char msg[512];
            size_t msglen;

            if (!semi) {
                break;
            }

            msglen = (size_t)(semi - g_tcp_context->stream_buffer) + 1;
            if (msglen >= sizeof(msg)) {
                msglen = sizeof(msg) - 1;
            }
            memcpy(msg, g_tcp_context->stream_buffer, msglen);
            msg[msglen] = '\0';
            tci_parse_text_response(client, msg);
            tci_tcp_consume_stream(msglen);
            continue;
        }

        {
            size_t frame_size = tci_tcp_binary_frame_size(g_tcp_context->stream_buffer,
                                                          g_tcp_context->stream_len);
            if (frame_size == 0 || g_tcp_context->stream_len < frame_size) {
                break;
            }

            tci_stage_audio_nolock(client,
                                   g_tcp_context->stream_buffer,
                                   frame_size);
            tci_tcp_consume_stream(frame_size);
        }
    }
    pthread_mutex_unlock(&g_tcp_context->lock);
}

static int callback_tci(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len) {
    (void)user;
    (void)lws_get_vhost(wsi);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    (void)lws_get_context(wsi);
#pragma GCC diagnostic pop
    const struct lws_protocols *prot = lws_get_protocol(wsi);
    tci_ws_context_t *ws_ctx = NULL;
    tci_client_t *client;

    if (prot && prot->user) {
        ws_ctx = (tci_ws_context_t *)prot->user;
    }

    if (!ws_ctx || !ws_ctx->client) {
        return 0;
    }

    client = ws_ctx->client;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        LOG_INFO("WebSocket connection established");
        CONN_DBG("WebSocket ESTABLISHED to %s:%d", client->host, client->port);
        pthread_mutex_lock(&ws_ctx->lock);
        ws_ctx->connected = 1;
        ws_ctx->wsi = wsi;
        pthread_mutex_unlock(&ws_ctx->lock);
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        LOG_WARN("WebSocket connection closed");
        CONN_DBG("WebSocket CLOSED (%s:%d)", client->host, client->port);
        pthread_mutex_lock(&ws_ctx->lock);
        ws_ctx->connected = 0;
        ws_ctx->wsi = NULL;
        pthread_mutex_unlock(&ws_ctx->lock);
        client->connected = 0;
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (len == 0) {
            break;
        }

        if (!lws_frame_is_binary(wsi) && in &&
            ((unsigned char *)in)[0] >= 32 && ((unsigned char *)in)[0] < 127) {
            char text_buf[256];
            int text_len = len < 255 ? (int)len : 255;
            memcpy(text_buf, in, (size_t)text_len);
            text_buf[text_len] = '\0';
            tci_parse_text_response(client, text_buf);
        } else {
            static int bin_rx_log = 0;
            if (++bin_rx_log <= 5 || (bin_rx_log % 200) == 0) {
                CONN_DBG("WS binary RX #%d len=%zu binary=%d",
                         bin_rx_log, len, lws_frame_is_binary(wsi) ? 1 : 0);
            }
            pthread_mutex_lock(&ws_ctx->lock);
            tci_stage_audio_nolock(client, (const unsigned char *)in, len);
            pthread_mutex_unlock(&ws_ctx->lock);
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        const char *err = (in && len > 0) ? (const char *)in : "unknown";
        LOG_ERROR("WebSocket connection error: %s", err);
        CONN_DBG("WebSocket CONNECTION_ERROR to %s:%d: %s", client->host, client->port, err);
        pthread_mutex_lock(&ws_ctx->lock);
        ws_ctx->connected = 0;
        ws_ctx->connect_failed = 1;
        ws_ctx->wsi = NULL;
        pthread_mutex_unlock(&ws_ctx->lock);
        client->connected = 0;
        break;
    }

    default:
        break;
    }

    return 0;
}

static int tci_probe_host_port(const char *host, int port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    char portstr[16];
    int sockfd = -1;
    int reachable = -1;

    if (!host || port <= 0) {
        return -1;
    }

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        CONN_DBG("FAILED: DNS/getaddrinfo for %s:%s", host, portstr);
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) {
            continue;
        }
        if (tci_tcp_connect_socket(sockfd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
            reachable = 0;
            close(sockfd);
            break;
        }
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);
    return reachable;
}

static int tci_tcp_connect_socket(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int flags;
    int rc;

    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    rc = connect(sockfd, addr, addrlen);
    if (rc == 0) {
        (void)fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
        return 0;
    }
    if (errno != EINPROGRESS) {
        return -1;
    }

    {
        struct pollfd pfd;
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);

        pfd.fd = sockfd;
        pfd.events = POLLOUT;
        rc = poll(&pfd, 1, TCI_TCP_CONNECT_TIMEOUT_MS);
        if (rc <= 0) {
            errno = (rc == 0) ? ETIMEDOUT : errno;
            return -1;
        }

        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0) {
            return -1;
        }
        if (so_error != 0) {
            errno = so_error;
            return -1;
        }
    }

    (void)fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

static void tci_tcp_context_free(void) {
    if (!g_tcp_context) {
        return;
    }

    if (g_tcp_context->stream_buffer) {
        free(g_tcp_context->stream_buffer);
    }
    if (g_tcp_context->audio_buffer) {
        free(g_tcp_context->audio_buffer);
    }
    pthread_mutex_destroy(&g_tcp_context->lock);
    free(g_tcp_context);
    g_tcp_context = NULL;
}

static int tci_tcp_connect(tci_client_t *client) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    char portstr[16];
    int sockfd = -1;

    if (!client) {
        return -1;
    }

    if (g_tcp_context) {
        tci_tcp_context_free();
    }

    g_tcp_context = calloc(1, sizeof(*g_tcp_context));
    if (!g_tcp_context) {
        return -1;
    }

    g_tcp_context->client = client;
    pthread_mutex_init(&g_tcp_context->lock, NULL);
    g_tcp_context->stream_size = 8192;
    g_tcp_context->audio_size = 8192;
    g_tcp_context->stream_buffer = malloc(g_tcp_context->stream_size);
    g_tcp_context->audio_buffer = malloc(g_tcp_context->audio_size);
    if (!g_tcp_context->stream_buffer || !g_tcp_context->audio_buffer) {
        tci_tcp_context_free();
        return -1;
    }

    snprintf(portstr, sizeof(portstr), "%d", client->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(client->host, portstr, &hints, &res) != 0) {
        CONN_DBG("FAILED: getaddrinfo for %s:%s", client->host, portstr);
        tci_tcp_context_free();
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) {
            continue;
        }
        if (tci_tcp_connect_socket(sockfd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
            break;
        }
        CONN_DBG("TCP connect attempt failed for %s:%d (%s)",
                 client->host, client->port, strerror(errno));
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd < 0) {
        CONN_DBG("FAILED: TCP connect to %s:%d timed out or refused", client->host, client->port);
        tci_tcp_context_free();
        return -1;
    }

    {
        int yes = 1;
        (void)setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    }

    client->socket_fd = sockfd;
    client->connected = 1;
    LOG_INFO("Connected to TCI radio at %s:%d via TCP/IP", client->host, client->port);
    CONN_DBG("SUCCESS: TCP/IP connected to %s:%d", client->host, client->port);
    return 0;
}

static int tci_ws_connect(tci_client_t *client) {
    if (!client) {
        return -1;
    }

    if (g_ws_context) {
        LOG_WARN("WebSocket context already exists, destroying");
        tci_client_disconnect(client);
    }

    g_ws_context = calloc(1, sizeof(*g_ws_context));
    if (!g_ws_context) {
        LOG_ERROR("Failed to allocate WebSocket context");
        return -1;
    }

    g_ws_context->client = client;
    pthread_mutex_init(&g_ws_context->lock, NULL);
    g_ws_context->rx_buffer = malloc(TCI_RX_BUFFER_SIZE);
    if (!g_ws_context->rx_buffer) {
        LOG_ERROR("Failed to allocate RX buffer");
        free(g_ws_context);
        g_ws_context = NULL;
        return -1;
    }

    {
        struct lws_context_creation_info info;
        static struct lws_protocols protocols[] = {
            { "tci", callback_tci, 0, 1024, 0, NULL, 0 },
            { "superchat", callback_tci, 0, 1024, 0, NULL, 0 },
            { "chat", callback_tci, 0, 1024, 0, NULL, 0 },
            { NULL, NULL, 0, 0, 0, NULL, 0 }
        };
        struct lws_client_connect_info ccinfo;

        memset(&info, 0, sizeof(info));
        info.port = CONTEXT_PORT_NO_LISTEN;
        protocols[0].user = g_ws_context;
        protocols[1].user = g_ws_context;
        protocols[2].user = g_ws_context;
        info.protocols = protocols;
        info.gid = -1;
        info.uid = -1;

        g_ws_context->context = lws_create_context(&info);
        if (!g_ws_context->context) {
            CONN_DBG("FAILED: could not create WebSocket context");
            LOG_ERROR("Failed to create WebSocket context");
            free(g_ws_context->rx_buffer);
            free(g_ws_context);
            g_ws_context = NULL;
            return -1;
        }

        memset(&ccinfo, 0, sizeof(ccinfo));
        ccinfo.context = g_ws_context->context;
        ccinfo.address = client->host;
        ccinfo.port = client->port;
        ccinfo.path = "/";
        ccinfo.protocol = "tci";
        ccinfo.host = client->host;
        ccinfo.origin = client->host;

        CONN_DBG("Initiating WebSocket handshake ws://%s:%d/ protocol=tci", client->host, client->port);
        g_ws_context->wsi = lws_client_connect_via_info(&ccinfo);
        if (!g_ws_context->wsi) {
            CONN_DBG("FAILED: lws_client_connect_via_info returned NULL for %s:%d", client->host, client->port);
            LOG_ERROR("Failed to initiate WebSocket connection to %s:%d", client->host, client->port);
            lws_context_destroy(g_ws_context->context);
            free(g_ws_context->rx_buffer);
            free(g_ws_context);
            g_ws_context = NULL;
            return -1;
        }

        CONN_DBG("Waiting for WebSocket handshake (up to ~5s) ...");
        for (int i = 0; i < 100 && !g_ws_context->connected && !g_ws_context->connect_failed; i++) {
            lws_service(g_ws_context->context, 50);
            usleep(10000);
            if ((i + 1) % 10 == 0) {
                CONN_DBG("  handshake poll %d/100, connected=%d failed=%d",
                         i + 1, g_ws_context->connected, g_ws_context->connect_failed);
            }
        }

        if (!g_ws_context->connected) {
            if (g_ws_context->connect_failed) {
                CONN_DBG("FAILED: WebSocket connection to %s:%d rejected or unreachable",
                         client->host, client->port);
            } else {
                CONN_DBG("FAILED: WebSocket connection timeout to %s:%d", client->host, client->port);
            }
            LOG_ERROR("WebSocket connection timeout");
            lws_context_destroy(g_ws_context->context);
            free(g_ws_context->rx_buffer);
            free(g_ws_context);
            g_ws_context = NULL;
            return -1;
        }

        /* Drain initial protocol/device announcements before subscribe */
        for (int i = 0; i < 20 && client->remote_protocol[0] == '\0'; i++) {
            lws_service(g_ws_context->context, 50);
            usleep(10000);
        }
    }

    client->connected = 1;
    LOG_INFO("Connected to TCI radio at %s:%d via WebSocket", client->host, client->port);
    CONN_DBG("SUCCESS: connected to TCI radio at %s:%d via WebSocket", client->host, client->port);
    return 0;
}

static int tci_tcp_send_command(tci_client_t *client, const char *command) {
    char buffer[512];
    size_t len;

    if (!client || client->socket_fd < 0 || !command) {
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", command);
    len = strlen(buffer);
    if (len == 0) {
        return -1;
    }
    if (buffer[len - 1] != ';') {
        if (len + 1 >= sizeof(buffer)) {
            return -1;
        }
        buffer[len++] = ';';
        buffer[len] = '\0';
    }

    {
        ssize_t sent = send(client->socket_fd, buffer, len, MSG_NOSIGNAL);
        if (sent < 0 || (size_t)sent != len) {
            LOG_ERROR("Failed to send TCP command: %s", command);
            CONN_DBG("FAILED to send TCP command: %s", command);
            client->connected = 0;
            return -1;
        }
    }

    LOG_DEBUG("TCI command sent via TCP: %s", buffer);
    CONN_DBG("TCP command sent: %s", buffer);
    return 0;
}

static int tci_ws_send_command(tci_client_t *client, const char *command) {
    char buffer[512];
    size_t len;
    unsigned char frame[512];
    int result;

    if (!client || !g_ws_context || !g_ws_context->wsi || !command) {
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", command);
    len = strlen(buffer);
    if (len == 0) {
        return -1;
    }
    if (buffer[len - 1] != ';') {
        if (len + 1 >= sizeof(buffer)) {
            return -1;
        }
        buffer[len++] = ';';
        buffer[len] = '\0';
    }

    if (len + LWS_PRE > sizeof(frame)) {
        LOG_ERROR("Command too long");
        return -1;
    }

    memcpy(&frame[LWS_PRE], buffer, len);
    pthread_mutex_lock(&g_ws_context->lock);
    result = lws_write(g_ws_context->wsi, &frame[LWS_PRE], len, LWS_WRITE_TEXT);
    pthread_mutex_unlock(&g_ws_context->lock);

    if (result < 0) {
        LOG_ERROR("Failed to send WebSocket command: %s", command);
        CONN_DBG("FAILED to send command: %s (connected=%d)", command, client->connected);
        client->connected = 0;
        return -1;
    }

    LOG_DEBUG("TCI command sent via WebSocket: %s", buffer);
    CONN_DBG("Command sent: %s", buffer);
    return 0;
}

static void tci_tcp_service(tci_client_t *client) {
    unsigned char chunk[8192];
    ssize_t n;
    struct pollfd pfd;

    if (!client || !g_tcp_context || client->socket_fd < 0 || !client->connected) {
        return;
    }

    pfd.fd = client->socket_fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) <= 0) {
        return;
    }

    n = recv(client->socket_fd, chunk, sizeof(chunk), 0);
    if (n == 0) {
        CONN_DBG("TCP connection closed by peer %s:%d", client->host, client->port);
        client->connected = 0;
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        CONN_DBG("TCP recv error on %s:%d: %s", client->host, client->port, strerror(errno));
        client->connected = 0;
        return;
    }

    pthread_mutex_lock(&g_tcp_context->lock);
    if (tci_tcp_append_stream(chunk, (size_t)n) == 0) {
        pthread_mutex_unlock(&g_tcp_context->lock);
        tci_tcp_parse_stream_buffer(client);
    } else {
        pthread_mutex_unlock(&g_tcp_context->lock);
    }
}

tci_client_t *tci_client_create(const char *host, int port, int sample_rate, const char *protocol) {
    tci_client_t *client;

    (void)sample_rate;

    client = malloc(sizeof(tci_client_t));
    if (!client) {
        return NULL;
    }

    memset(client, 0, sizeof(*client));
    client->socket_fd = -1;
    client->connected = 0;
    client->buffer_head = 0;
    client->buffer_count = 0;
    client->buffer_size = IQ_BUFFER_SIZE;
    client->transport = tci_parse_transport(protocol);

    strncpy(client->host, host, sizeof(client->host) - 1);
    client->port = port;

    client->iq_buffer = malloc(IQ_BUFFER_SIZE * sizeof(float complex));
    if (!client->iq_buffer) {
        free(client);
        return NULL;
    }

    LOG_DEBUG("TCI client created for %s:%d via %s", host, port, tci_transport_name(client->transport));
    CONN_DBG("TCI client created for %s:%d via %s", host, port, tci_transport_name(client->transport));
    return client;
}

int tci_client_connect(tci_client_t *client) {
    if (!client) {
        return -1;
    }

    CONN_DBG("Connecting via %s to %s:%d ...",
             tci_transport_name(client->transport), client->host, client->port);

    if (tci_probe_host_port(client->host, client->port) < 0) {
        CONN_DBG("FAILED: no TCP listener on %s:%d", client->host, client->port);
        CONN_DBG("Thetis: Setup -> CAT/TCI -> check 'TCI Server Running'");
        CONN_DBG("Thetis: set bind address to 0.0.0.0:50001 (not 127.0.0.1:50001)");
        CONN_DBG("Windows: allow inbound TCP port %d in firewall", client->port);
        return -1;
    }

    CONN_DBG("TCP port %d is reachable on %s, proceeding with %s handshake",
             client->port, client->host, tci_transport_name(client->transport));

    if (client->transport == TCI_TRANSPORT_WEBSOCKET) {
        return tci_ws_connect(client);
    }
    return tci_tcp_connect(client);
}

int tci_send_command(tci_client_t *client, const char *command) {
    if (!client || !client->connected) {
        return -1;
    }

    if (client->transport == TCI_TRANSPORT_WEBSOCKET) {
        return tci_ws_send_command(client, command);
    }
    return tci_tcp_send_command(client, command);
}

int tci_subscribe_audio_stream(tci_client_t *client, int vfo) {
    char cmd[64];

    if (!client || !client->connected) {
        return -1;
    }

    CONN_DBG("Subscribing via audio_start:%d on %s:%d (demodulated RX audio)",
             vfo, client->host, client->port);

    if (tci_send_command(client, "audio_samplerate:48000") < 0) {
        CONN_DBG("WARN: audio_samplerate:48000 not acknowledged (continuing)");
    }
    if (tci_send_command(client, "audio_stream_sample_type:3") < 0) {
        CONN_DBG("WARN: audio_stream_sample_type:3 not acknowledged (continuing)");
    }
    if (tci_send_command(client, "audio_stream_channels:2") < 0) {
        CONN_DBG("WARN: audio_stream_channels:2 not acknowledged (continuing)");
    }
    if (tci_send_command(client, "audio_stream_samples:512") < 0) {
        CONN_DBG("WARN: audio_stream_samples:512 not acknowledged (continuing)");
    }
    if (tci_send_command(client, "tx_stream_audio_buffering:0") < 0) {
        CONN_DBG("WARN: tx_stream_audio_buffering:0 not acknowledged (continuing)");
    }

    snprintf(cmd, sizeof(cmd), "audio_start:%d", vfo);
    if (tci_send_command(client, cmd) < 0) {
        CONN_DBG("FAILED: %s command rejected", cmd);
        return -1;
    }

    client->iq_vfo = vfo;
    g_stream_audio_only = 1;
    LOG_INFO("Stream subscription via %s (demodulated RX audio)", cmd);
    CONN_DBG("Audio stream subscription sent OK");
    return 0;
}

static int tci_subscribe_wideband_iq_stream(tci_client_t *client, int vfo) {
    char cmd[64];

    if (!client || !client->connected) {
        return -1;
    }

    CONN_DBG("Subscribing via iq_start:%d on %s:%d (wideband I/Q)",
             vfo, client->host, client->port);

    if (tci_send_command(client, "iq_samplerate:48000") < 0) {
        CONN_DBG("WARN: iq_samplerate:48000 not acknowledged (continuing)");
    }

    snprintf(cmd, sizeof(cmd), "iq_start:%d", vfo);
    if (tci_send_command(client, cmd) < 0) {
        CONN_DBG("FAILED: %s command rejected", cmd);
        return -1;
    }

    client->iq_vfo = vfo;
    g_stream_audio_only = 0;
    LOG_INFO("I/Q stream subscription requested via %s", cmd);
    CONN_DBG("Wideband I/Q stream subscription sent OK");
    return 0;
}

int tci_stream_is_audio_only(const tci_client_t *client)
{
    (void)client;
    return g_stream_audio_only;
}

static int tci_looks_like_deskhpsdr(const tci_client_t *client)
{
    /* deskHPSDR greets as ExpertSDR3 / SunSDR2QRP for CAT compatibility. */
    if (!client) {
        return 0;
    }
    if (strcasecmp(client->remote_device, "SunSDR2QRP") == 0 &&
        strstr(client->remote_protocol, "ExpertSDR3") != NULL) {
        return 1;
    }
    return 0;
}

int tci_stop_streams(tci_client_t *client, int vfo)
{
    char cmd[64];
    int rc = 0;

    if (!client || !client->connected) {
        return -1;
    }
    if (vfo < 0) {
        vfo = 0;
    }

    snprintf(cmd, sizeof(cmd), "iq_stop:%d", vfo);
    if (tci_send_command(client, cmd) < 0) {
        CONN_DBG("iq_stop failed or unsupported (continuing)");
        rc = -1;
    }
    snprintf(cmd, sizeof(cmd), "audio_stop:%d", vfo);
    if (tci_send_command(client, cmd) < 0) {
        CONN_DBG("audio_stop failed or unsupported (continuing)");
        rc = -1;
    }
    return rc;
}

int tci_resubscribe_stream(tci_client_t *client, int vfo)
{
    if (!client || !client->connected) {
        return -1;
    }
    if (vfo < 0) {
        vfo = client->iq_vfo;
    }
    if (vfo < 0) {
        vfo = 0;
    }

    LOG_INFO("TCI resubscribe stream VFO %d mode=%s",
             vfo,
             g_stream_mode_pref == TCI_STREAM_MODE_AUDIO ? "AUDIO" : "IQ");
    CONN_DBG("resubscribe: stop both streams, clear buffer, subscribe %s",
             g_stream_mode_pref == TCI_STREAM_MODE_AUDIO ? "audio" : "iq");

    (void)tci_stop_streams(client, vfo);
    tci_clear_sample_buffer(client);
    return tci_subscribe_iq_stream(client, vfo);
}

int tci_subscribe_iq_stream(tci_client_t *client, int vfo) {
    int rc;

    if (!client || !client->connected) {
        CONN_DBG("Cannot subscribe stream: client=%p connected=%d",
                 (void *)client, client ? client->connected : 0);
        return -1;
    }

    client->iq_vfo = vfo;
    CONN_DBG("Active stream VFO set to %d (protocol='%s' device='%s' pref=%s)",
             vfo, client->remote_protocol, client->remote_device,
             g_stream_mode_pref == TCI_STREAM_MODE_AUDIO ? "AUDIO" : "IQ");

    /* User chose demodulated audio — do not try IQ. */
    if (g_stream_mode_pref == TCI_STREAM_MODE_AUDIO) {
        CONN_DBG("Stream preference AUDIO — audio_start only");
        g_stream_audio_only = 1;
        return tci_subscribe_audio_stream(client, vfo);
    }

    /*
     * Preference IQ: true complex baseband. Fall back to audio only if the
     * command send fails (not on silent stall — that is handled elsewhere).
     */
    if (tci_uses_wideband_iq(client)) {
        if (client->remote_protocol[0] != '\0') {
            CONN_DBG("Trying iq_start for remote protocol '%s' / device '%s'",
                     client->remote_protocol, client->remote_device);
        }
        if (tci_looks_like_deskhpsdr(client)) {
            LOG_INFO("TCI peer looks like deskHPSDR — requesting iq_start (complex IQ)");
        }
        rc = tci_subscribe_wideband_iq_stream(client, vfo);
        if (rc == 0) {
            g_stream_audio_only = 0;
            return 0;
        }
        LOG_WARN("iq_start failed (rc=%d) — falling back to audio_start", rc);
        CONN_DBG("iq_start failed, falling back to demodulated audio_start");
    }

    CONN_DBG("Using audio_start demodulated audio stream");
    g_stream_audio_only = 1;
    return tci_subscribe_audio_stream(client, vfo);
}

int tci_read_iq_samples(tci_client_t *client) {
    unsigned char *buffer = NULL;
    size_t bytes_to_process = 0;
    size_t consumed = 0;
    int processed = 0;

    if (!client) {
        return 0;
    }

    if (client->transport == TCI_TRANSPORT_WEBSOCKET) {
        if (!g_ws_context) {
            return 0;
        }
        pthread_mutex_lock(&g_ws_context->lock);
        buffer = g_ws_context->rx_buffer;
        bytes_to_process = g_ws_context->rx_buffer_len;
        if (bytes_to_process == 0) {
            pthread_mutex_unlock(&g_ws_context->lock);
            return 0;
        }
        processed = tci_process_audio_payload(client, buffer, bytes_to_process, &consumed);
        if (consumed > 0 && consumed < bytes_to_process) {
            memmove(buffer, buffer + consumed, bytes_to_process - consumed);
            g_ws_context->rx_buffer_len = bytes_to_process - consumed;
        } else if (consumed >= bytes_to_process) {
            g_ws_context->rx_buffer_len = 0;
        }
        /* else consumed==0: keep incomplete frame for next read */
        pthread_mutex_unlock(&g_ws_context->lock);
    } else {
        if (!g_tcp_context) {
            return 0;
        }
        pthread_mutex_lock(&g_tcp_context->lock);
        buffer = g_tcp_context->audio_buffer;
        bytes_to_process = g_tcp_context->audio_len;
        if (bytes_to_process == 0) {
            pthread_mutex_unlock(&g_tcp_context->lock);
            return 0;
        }
        processed = tci_process_audio_payload(client, buffer, bytes_to_process, &consumed);
        if (consumed > 0 && consumed < bytes_to_process) {
            memmove(buffer, buffer + consumed, bytes_to_process - consumed);
            g_tcp_context->audio_len = bytes_to_process - consumed;
        } else if (consumed >= bytes_to_process) {
            g_tcp_context->audio_len = 0;
        }
        pthread_mutex_unlock(&g_tcp_context->lock);
    }

    if (processed > 0) {
        static int parse_count = 0;
        if (++parse_count <= 3 || parse_count % 100 == 0) {
            LOG_DEBUG("tci_read: processed %d IQ pairs from %zu bytes via %s",
                      processed, bytes_to_process, tci_transport_name(client->transport));
        }
    }

    return processed;
}

void tci_client_disconnect(tci_client_t *client) {
    if (!client) {
        return;
    }

    if (g_ws_context) {
        if (g_ws_context->context) {
            lws_context_destroy(g_ws_context->context);
        }
        if (g_ws_context->rx_buffer) {
            free(g_ws_context->rx_buffer);
        }
        pthread_mutex_destroy(&g_ws_context->lock);
        free(g_ws_context);
        g_ws_context = NULL;
    }

    if (g_tcp_context) {
        tci_tcp_context_free();
    }

    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    client->connected = 0;
    LOG_INFO("Disconnected from TCI radio");
    CONN_DBG("Disconnected from TCI radio");
}

int tci_get_iq_samples(tci_client_t *client, float complex *samples, int count) {
    int available;
    int tail;
    int i;

    if (!client || !samples || count <= 0) {
        return 0;
    }

    available = client->buffer_count;
    if (count > available) {
        count = available;
    }

    tail = (client->buffer_head - client->buffer_count + client->buffer_size) % client->buffer_size;

    for (i = 0; i < count; i++) {
        samples[i] = client->iq_buffer[(tail + i) % client->buffer_size];
    }

    client->buffer_count -= count;
    return count;
}

int tci_buffer_available(tci_client_t *client) {
    if (!client) {
        return 0;
    }
    return client->buffer_count;
}

int tci_is_connected(tci_client_t *client) {
    if (!client) {
        return 0;
    }
    return client->connected;
}

void tci_client_destroy(tci_client_t *client) {
    if (!client) {
        return;
    }

    tci_client_disconnect(client);

    if (client->iq_buffer) {
        free(client->iq_buffer);
    }

    free(client);
    LOG_DEBUG("TCI client destroyed");
}

long long tci_get_center_frequency(tci_client_t *client) {
    if (!client) {
        return 0;
    }
    return client->center_frequency;
}

int tci_request_dds_frequency(tci_client_t *client, int receiver)
{
    char cmd[64];
    if (!client || !client->connected) {
        return -1;
    }
    /* Query: dds:<rx>  → radio replies dds:<rx>,<Hz>; */
    snprintf(cmd, sizeof(cmd), "dds:%d", receiver);
    return tci_send_command(client, cmd);
}

int tci_request_vfo_frequency(tci_client_t *client, int vfo, int channel) {
    char cmd[64];

    if (!client || !client->connected) {
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "vfo:%d,%d", vfo, channel);
    return tci_send_command(client, cmd);
}

void tci_service_websocket(void) {
    if (g_ws_context && g_ws_context->context) {
        lws_service(g_ws_context->context, 10);
    }
    if (g_tcp_context && g_tcp_context->client) {
        tci_tcp_service(g_tcp_context->client);
    }
}