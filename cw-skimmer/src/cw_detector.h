#ifndef CW_DETECTOR_H
#define CW_DETECTOR_H

#include <complex.h>
#include "bayesian_tree.h"

typedef struct {
    float frequency;
    float tone_purity;
    float snr_db;
    float bandwidth;
    float confidence;
    int valid;
} cw_signal_t;

typedef struct {
    bayesian_classifier_t *classifier;
    
    // Per-channel signal state
    float *channel_powers;
    float *channel_frequencies;
    float *smoothed_powers;
    float noise_floor;
    
    int num_channels;
    int sample_rate;
    
    // Detection parameters
    int detection_threshold;
    float min_snr_db;
    int hysteresis_count;
    int min_detection_duration_ms;
} cw_detector_t;

/**
 * Create CW detector
 * @param sample_rate 48000 Hz
 * @param num_channels Number of frequency channels (typically 480 for 48kHz / 100Hz resolution)
 * @param threshold Detection confidence threshold (0-100)
 * @return Initialized detector
 */
cw_detector_t *cw_detector_create(int sample_rate, int num_channels, int threshold);

/**
 * Analyze complex I/Q samples for CW signals (standalone FFT path).
 * @param detector Detector instance
 * @param iq_samples Complex I/Q input samples
 * @param count Number of samples
 * @return 0 on success
 */
int cw_detector_analyze(cw_detector_t *detector, float complex *iq_samples, int count);

/**
 * Update detector channel powers from a pre-computed FFT power spectrum
 * (e.g. from audio_processor_get_power_spectrum — avoids duplicate FFT work).
 * @param power_db dB power array, fftshifted: [0]=-fs/2 .. [n-1]=+fs/2
 * @param num_bins Number of FFT bins
 * @param sample_rate Sample rate in Hz
 * @return 0 on success
 */
int cw_detector_feed_spectrum(cw_detector_t *detector, const float *power_db,
                              int num_bins, int sample_rate);

/**
 * Get detected signals
 * @param detector Detector instance
 * @param signals Output array
 * @param max_signals Maximum signals to return
 * @return Actual number of signals detected
 */
int cw_detector_get_signals(cw_detector_t *detector, cw_signal_t *signals, int max_signals);

/**
 * Get signal at specific channel
 */
cw_signal_t cw_detector_get_channel_signal(cw_detector_t *detector, int channel);

/**
 * Reset detector state
 */
void cw_detector_reset(cw_detector_t *detector);

/**
 * Destroy detector
 */
void cw_detector_destroy(cw_detector_t *detector);

/**
 * Get current noise floor estimate
 */
float cw_detector_get_noise_floor(cw_detector_t *detector);

#endif
