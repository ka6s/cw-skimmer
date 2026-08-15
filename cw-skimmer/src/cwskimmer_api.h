/**
 * @file cwskimmer_api.h
 * @brief Thread-safe C/Qt bridge API for CW Skimmer detector
 *
 * This API exposes the detector functionality in a thread-safe manner
 * suitable for use from Qt applications. It provides callbacks for
 * reporting detected signals, spots, and log messages.
 */

#ifndef CWSKIMMER_API_H
#define CWSKIMMER_API_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Signal detection event */
typedef struct {
    float frequency;        /* Absolute RF frequency in Hz */
    float freq_offset_hz;   /* Offset from tuned center (±24 kHz passband) */
    float snr_db;
    float confidence;
    float tone_purity;
    float bandwidth;
    time_t timestamp;
} cwskimmer_signal_t;

/* Spot report event */
typedef struct {
    char callsign[16];
    float frequency_hz;
    float snr_db;
    float confidence;
    time_t timestamp;
    char mode[8];
} cwskimmer_spot_t;

/* Detector statistics */
typedef struct {
    int num_signals;
    float noise_floor_db;
    float avg_peak_snr_db;  /* Mean SNR of FFT peaks in latest I/Q block */
    float peak_snr_db;      /* Strongest FFT peak SNR in latest I/Q block */
    int spectrum_peak_count;
    int buffer_fill;
    int connected;
    int samples_processed;
    float cpu_usage;
    int queue_size;
} cwskimmer_stats_t;

/* Live decode display update */
typedef struct {
    char text[64];
    char partial_morse[32];
    char new_letter;        /* Single letter/space just decoded; 0 if none */
    float frequency_hz;     /* Absolute RF frequency in Hz */
    float freq_offset_hz;   /* Offset from tuned center for display alignment */
    float text_confidence;  /* Message validator score 0.0-1.0 for display coloring */
} cwskimmer_decode_t;

/* Spectrum data for visualization */
typedef struct {
    float *power_spectrum;  /* Power in dB for each frequency bin */
    int num_bins;           /* Number of bins (typically 480 for 48kHz/100Hz) */
    float center_frequency; /* Center frequency in Hz */
    float bin_width;        /* Frequency width of each bin in Hz */
} cwskimmer_spectrum_t;

/* Opaque detector handle */
typedef struct cwskimmer_detector cwskimmer_detector_t;

/* Callback function types */
typedef void (*cwskimmer_signal_callback)(const cwskimmer_signal_t *signal, void *userdata);
typedef void (*cwskimmer_spot_callback)(const cwskimmer_spot_t *spot, void *userdata);
typedef void (*cwskimmer_log_callback)(const char *message, int level, void *userdata);
typedef void (*cwskimmer_stats_callback)(const cwskimmer_stats_t *stats, void *userdata);
typedef void (*cwskimmer_spectrum_callback)(const cwskimmer_spectrum_t *spectrum, void *userdata);
typedef void (*cwskimmer_decode_callback)(const cwskimmer_decode_t *decode, void *userdata);

/**
 * Create a detector instance
 * @param config_file Path to config file (or NULL for defaults)
 * @return Handle to detector, or NULL on error
 */
cwskimmer_detector_t *cwskimmer_detector_create(const char *config_file);

/**
 * Destroy detector instance
 */
void cwskimmer_detector_destroy(cwskimmer_detector_t *detector);

/**
 * Register callback for signal detections
 */
void cwskimmer_set_signal_callback(cwskimmer_detector_t *detector,
                                   cwskimmer_signal_callback callback,
                                   void *userdata);

/**
 * Register callback for spot reports
 */
void cwskimmer_set_spot_callback(cwskimmer_detector_t *detector,
                                 cwskimmer_spot_callback callback,
                                 void *userdata);

/**
 * Register callback for log messages
 */
void cwskimmer_set_log_callback(cwskimmer_detector_t *detector,
                                cwskimmer_log_callback callback,
                                void *userdata);

/**
 * Register callback for statistics updates
 */
void cwskimmer_set_stats_callback(cwskimmer_detector_t *detector,
                                  cwskimmer_stats_callback callback,
                                  void *userdata);

/**
 * Register callback for spectrum updates (for visualization)
 */
void cwskimmer_set_spectrum_callback(cwskimmer_detector_t *detector,
                                     cwskimmer_spectrum_callback callback,
                                     void *userdata);

/**
 * Register callback for live CW decode text updates
 */
void cwskimmer_set_decode_callback(cwskimmer_detector_t *detector,
                                   cwskimmer_decode_callback callback,
                                   void *userdata);

/**
 * Start detection (begins main processing loop)
 * This should be called from a separate thread
 * @return 0 on success, -1 on error
 */
int cwskimmer_start(cwskimmer_detector_t *detector);

/**
 * Stop detection (signals main loop to exit)
 */
void cwskimmer_stop(cwskimmer_detector_t *detector);

/**
 * Check if detector is running
 */
int cwskimmer_is_running(cwskimmer_detector_t *detector);

/**
 * Check if the detection loop thread is still inside cwskimmer_start()
 */
int cwskimmer_is_loop_active(cwskimmer_detector_t *detector);

/**
 * Get current statistics
 */
cwskimmer_stats_t cwskimmer_get_stats(cwskimmer_detector_t *detector);

/**
 * Update configuration parameter
 */
int cwskimmer_config_set(cwskimmer_detector_t *detector,
                         const char *key,
                         const char *value);

/**
 * Get configuration parameter
 */
const char *cwskimmer_config_get(cwskimmer_detector_t *detector,
                                 const char *key);

/**
 * Save the rolling IQ capture buffer to disk for offline replay tests.
 * @param mark_freq_offset_hz Frequency offset (Hz) the user marked on the waterfall
 * @param expected_text Expected decode, e.g. "CQ" or "CQ DE K1ABC"
 * @param notes Optional free-form notes
 * @return 0 on success, -1 on error
 */
/**
 * Snapshot the current 20-second IQ ring buffer (call on Shift+click before the
 * capture dialog). save_capture writes this frozen copy if present.
 */
int cwskimmer_freeze_capture(cwskimmer_detector_t *detector);

void cwskimmer_clear_frozen_capture(cwskimmer_detector_t *detector);

int cwskimmer_save_capture(cwskimmer_detector_t *detector,
                           const char *path,
                           float mark_freq_offset_hz,
                           const char *expected_text,
                           const char *notes);

/**
 * Replay a .cwcap file through the detector/decode pipeline (offline test).
 * Prints decode results to stdout. Returns 0 if expected_text appears in output.
 */
int cwskimmer_replay_capture_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CWSKIMMER_API_H */
