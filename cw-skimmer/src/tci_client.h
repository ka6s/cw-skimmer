#ifndef TCI_CLIENT_H
#define TCI_CLIENT_H

#include <complex.h>

typedef enum {
    TCI_TRANSPORT_TCP = 0,
    TCI_TRANSPORT_WEBSOCKET = 1
} tci_transport_t;

typedef struct {
    int socket_fd;
    char host[256];
    int port;
    int connected;
    tci_transport_t transport;

    // I/Q sample buffer
    float complex *iq_buffer;
    int buffer_size;
    int buffer_head;
    int buffer_count;

    // Radio frequency for the I/Q stream we subscribed to (VFO/DDS index)
    int iq_vfo;
    long long center_frequency;  // Hz

    // Announced by radio during TCI handshake (e.g. "ExpertSDR3", "Thetis")
    char remote_protocol[64];
    char remote_device[64];
} tci_client_t;

/**
 * Initialize TCI client
 * @param host Radio host IP/hostname
 * @param port Radio TCI port (default 50001)
 * @param sample_rate Expected sample rate (48000)
 * @param protocol Transport: "tcp" (default) or "websocket"
 * @return Initialized client structure
 */
tci_client_t *tci_client_create(const char *host, int port, int sample_rate, const char *protocol);

/**
 * Connect to radio via TCI
 * @param client TCI client
 * @return 0 on success, -1 on error
 */
int tci_client_connect(tci_client_t *client);

/** Preferred TCI binary stream: complex IQ or demodulated stereo audio. */
typedef enum {
    TCI_STREAM_MODE_IQ = 0,
    TCI_STREAM_MODE_AUDIO = 1
} tci_stream_mode_t;

/**
 * Set preferred stream mode (used at subscribe and on live switch).
 * Default is TCI_STREAM_MODE_IQ.
 */
void tci_set_stream_mode_preference(tci_stream_mode_t mode);
tci_stream_mode_t tci_get_stream_mode_preference(void);

/**
 * Subscribe to the preferred stream (IQ or audio) from a VFO.
 * @param client TCI client
 * @param vfo VFO index (0 for main)
 * @return 0 on success, -1 on error
 */
int tci_subscribe_iq_stream(tci_client_t *client, int vfo);

/**
 * Demodulated RX audio (audio_start). L/R framed like IQ; mono for spectrum.
 */
int tci_subscribe_audio_stream(tci_client_t *client, int vfo);

/**
 * Stop both IQ and audio streams on the server (best-effort).
 */
int tci_stop_streams(tci_client_t *client, int vfo);

/**
 * Apply current preference: stop streams, clear buffer, subscribe again.
 * Safe to call while connected (live switch).
 */
int tci_resubscribe_stream(tci_client_t *client, int vfo);

/** Drop buffered samples (e.g. after stream mode change). */
void tci_clear_sample_buffer(tci_client_t *client);

/**
 * 1 if the active stream is audio_start (not wideband IQ).
 */
int tci_stream_is_audio_only(const tci_client_t *client);

/**
 * Read available I/Q samples from socket
 * Returns number of new samples available
 */
int tci_read_iq_samples(tci_client_t *client);

/**
 * Get available I/Q samples from buffer (circular buffer read)
 * @param client TCI client
 * @param samples Output array
 * @param count Maximum samples to read
 * @return Number of samples actually read
 */
int tci_get_iq_samples(tci_client_t *client, float complex *samples, int count);

/**
 * Get current buffer fill level
 */
int tci_buffer_available(tci_client_t *client);

/**
 * Disconnect and cleanup
 */
void tci_client_destroy(tci_client_t *client);

/**
 * Send TCI command
 * @return 0 on success
 */
int tci_send_command(tci_client_t *client, const char *command);

/**
 * Disconnect from radio
 */
void tci_client_disconnect(tci_client_t *client);

/**
 * Get radio's center frequency (from VFO)
 */
long long tci_get_center_frequency(tci_client_t *client);

/**
 * Check if the TCI client is currently connected
 */
int tci_is_connected(tci_client_t *client);

/**
 * Request current VFO frequency from the radio (triggers frequency update)
 * @param client TCI client
 * @param vfo VFO index (typically 0)
 * @param channel Channel (typically 0)
 * @return 0 on success, -1 on error
 */
int tci_request_vfo_frequency(tci_client_t *client, int vfo, int channel);

/**
 * Request current DDS (IQ LO) frequency — preferred center for wideband IQ stream.
 */
int tci_request_dds_frequency(tci_client_t *client, int receiver);

/**
 * Service transport event loop (WebSocket poll or TCP recv)
 * Must be called periodically from the main detection loop
 */
void tci_service_websocket(void);

#endif