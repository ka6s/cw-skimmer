#ifndef SPOT_REPORTER_H
#define SPOT_REPORTER_H

#include <time.h>

typedef struct {
    char callsign[16];
    float frequency_hz;
    float snr_db;
    float confidence;
    unsigned long timestamp;
    char mode[8];
} spot_t;

typedef struct {
    int socket_fd;
    char host[256];
    int port;
    int connected;
    
    char callsign[64];
    
    // Retry queue
    spot_t *retry_queue;
    int retry_head;
    int retry_tail;
    int queue_size;
    int max_queue_size;
    time_t last_connect_attempt;
} spot_reporter_t;

/**
 * Create spot reporter
 * @param host Telnet server host
 * @param port Telnet server port
 * @param callsign Reporter's callsign
 * @return Initialized reporter
 */
spot_reporter_t *spot_reporter_create(const char *host, int port, const char *callsign);

/**
 * Connect to telnet server
 * @return 0 on success, -1 on failure
 */
int spot_reporter_connect(spot_reporter_t *reporter);

/**
 * Submit a spot report
 * @return 0 on success, -1 on failure (will be queued for retry)
 */
int spot_reporter_submit_spot(spot_reporter_t *reporter, const spot_t *spot);

/**
 * Process retry queue (attempt to resend failed spots)
 * @return Number of spots successfully retransmitted
 */
int spot_reporter_process_retries(spot_reporter_t *reporter);

/**
 * Check connection status
 */
int spot_reporter_is_connected(spot_reporter_t *reporter);

/**
 * Reconnect if disconnected
 */
int spot_reporter_reconnect(spot_reporter_t *reporter);

/**
 * Get current retry queue length
 */
int spot_reporter_queue_length(spot_reporter_t *reporter);

/**
 * Destroy reporter
 */
void spot_reporter_destroy(spot_reporter_t *reporter);

#endif
