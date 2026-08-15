#include "spot_reporter.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#define RETRY_QUEUE_MAX 1000
#define SPOT_CONNECT_TIMEOUT_MS 100
#define SPOT_CONNECT_MIN_INTERVAL_SEC 10

static int spot_queue_spot(spot_reporter_t *reporter, const spot_t *spot)
{
    if (!reporter || !spot || reporter->queue_size >= reporter->max_queue_size) {
        return -1;
    }

    reporter->retry_queue[reporter->retry_tail] = *spot;
    reporter->retry_tail = (reporter->retry_tail + 1) % reporter->max_queue_size;
    reporter->queue_size++;
    LOG_DEBUG("Spot queued for retry (queue size: %d)", reporter->queue_size);
    return 0;
}

static int spot_connect_socket(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
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
        (void)fcntl(sockfd, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) {
        return -1;
    }

    {
        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLOUT;
        rc = poll(&pfd, 1, SPOT_CONNECT_TIMEOUT_MS);
        if (rc <= 0) {
            return -1;
        }

        {
            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0) {
                return -1;
            }
            if (so_error != 0) {
                errno = so_error;
                return -1;
            }
        }
    }

    (void)fcntl(sockfd, F_SETFL, flags);
    return 0;
}

spot_reporter_t *spot_reporter_create(const char *host, int port, const char *callsign) {
    spot_reporter_t *reporter = malloc(sizeof(spot_reporter_t));
    if (!reporter) return NULL;
    
    reporter->socket_fd = -1;
    reporter->connected = 0;
    reporter->max_queue_size = RETRY_QUEUE_MAX;
    reporter->retry_head = 0;
    reporter->retry_tail = 0;
    reporter->queue_size = 0;
    reporter->last_connect_attempt = 0;
    
    strncpy(reporter->host, host, sizeof(reporter->host) - 1);
    strncpy(reporter->callsign, callsign, sizeof(reporter->callsign) - 1);
    reporter->port = port;
    
    reporter->retry_queue = malloc(RETRY_QUEUE_MAX * sizeof(spot_t));
    if (!reporter->retry_queue) {
        free(reporter);
        return NULL;
    }
    
    LOG_DEBUG("Spot reporter created for %s:%d as %s", host, port, callsign);
    return reporter;
}

int spot_reporter_connect(spot_reporter_t *reporter) {
    if (!reporter) return -1;
    
    if (reporter->connected) return 0;
    
    reporter->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (reporter->socket_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(reporter->port);

    if (inet_pton(AF_INET, reporter->host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(reporter->host);
        if (!he) {
            LOG_ERROR("Failed to resolve host: %s", reporter->host);
            close(reporter->socket_fd);
            reporter->socket_fd = -1;
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }
    
    if (spot_connect_socket(reporter->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_WARN("Connection to spot server failed: %s:%d (%s)",
                 reporter->host, reporter->port, strerror(errno));
        close(reporter->socket_fd);
        reporter->socket_fd = -1;
        return -1;
    }
    
    // Set non-blocking mode
    int flags = fcntl(reporter->socket_fd, F_GETFL, 0);
    fcntl(reporter->socket_fd, F_SETFL, flags | O_NONBLOCK);
    
    reporter->connected = 1;
    LOG_INFO("Connected to spot server at %s:%d", reporter->host, reporter->port);
    
    return 0;
}

int spot_reporter_submit_spot(spot_reporter_t *reporter, const spot_t *spot) {
    if (!reporter || !spot) return -1;
    
    if (!reporter->connected) {
        /* Never block the DSP/decode thread on spot-server TCP connect. */
        return spot_queue_spot(reporter, spot);
    }
    
    // Format spot message in RBN-compatible format
    // Format: <callsign> <frequency> <mode> <snr> <drift> <? <db>> <date> <time> <?> <power>
    char message[256];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    snprintf(message, sizeof(message),
             "%s %f CW %+.1f 0 - %04d-%02d-%02d %02d:%02d:%02d\n",
             spot->callsign,
             spot->frequency_hz / 1000.0,
             spot->snr_db,
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec);
    
    int bytes_sent = send(reporter->socket_fd, message, strlen(message), MSG_DONTWAIT);
    
    if (bytes_sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_WARN("Failed to send spot: %s", strerror(errno));
            reporter->connected = 0;
            
            spot_queue_spot(reporter, spot);
            return -1;
        }
    }
    
    LOG_DEBUG("Spot submitted: %s @ %.1f kHz SNR: %.1f dB",
              spot->callsign, spot->frequency_hz / 1000.0, spot->snr_db);
    
    return 0;
}

int spot_reporter_process_retries(spot_reporter_t *reporter) {
    time_t now;

    if (!reporter || reporter->queue_size == 0) {
        return 0;
    }

    now = time(NULL);
    if (!reporter->connected) {
        if (reporter->last_connect_attempt != 0 &&
            (now - reporter->last_connect_attempt) < SPOT_CONNECT_MIN_INTERVAL_SEC) {
            return 0;
        }
        reporter->last_connect_attempt = now;
        if (spot_reporter_connect(reporter) < 0) {
            return 0;
        }
    }
    
    int resent = 0;
    while (reporter->queue_size > 0) {
        spot_t *spot = &reporter->retry_queue[reporter->retry_head];
        
        if (spot_reporter_submit_spot(reporter, spot) == 0) {
            reporter->retry_head = (reporter->retry_head + 1) % reporter->max_queue_size;
            reporter->queue_size--;
            resent++;
        } else {
            break;
        }
    }
    
    if (resent > 0) {
        LOG_INFO("Resubmitted %d queued spots, %d remaining",
                 resent, reporter->queue_size);
    }
    
    return resent;
}

int spot_reporter_is_connected(spot_reporter_t *reporter) {
    if (!reporter) return 0;
    return reporter->connected;
}

int spot_reporter_reconnect(spot_reporter_t *reporter) {
    if (!reporter) return -1;
    
    if (reporter->socket_fd >= 0) {
        close(reporter->socket_fd);
        reporter->socket_fd = -1;
    }
    
    reporter->connected = 0;
    return spot_reporter_connect(reporter);
}

int spot_reporter_queue_length(spot_reporter_t *reporter) {
    if (!reporter) return 0;
    return reporter->queue_size;
}

void spot_reporter_destroy(spot_reporter_t *reporter) {
    if (!reporter) return;
    
    if (reporter->socket_fd >= 0) {
        close(reporter->socket_fd);
    }
    
    if (reporter->retry_queue) {
        free(reporter->retry_queue);
    }
    
    free(reporter);
    LOG_DEBUG("Spot reporter destroyed");
}
