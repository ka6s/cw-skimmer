#ifndef SIGNAL_ANALYZER_H
#define SIGNAL_ANALYZER_H

#include <complex.h>

typedef struct {
    float freq_stability;      // 0-1: how stable the frequency is
    float tone_purity;         // 0-1: how pure the sine wave is
    float keying_regularity;   // 0-1: how consistent the keying is
    float snr_db;              // dB
    float envelope_shape;      // 0-1: sharpness of keying edges
    float bandwidth_hz;        // Estimated bandwidth
} signal_features_t;

typedef struct {
    // Complex signal history for analysis
    float complex *signal_buffer;
    int buffer_size;
    int buffer_index;
    
    // Statistics
    float mean_magnitude;
    float variance;
    float prev_magnitude;
    
    int sample_rate;
} signal_analyzer_t;

/**
 * Create signal analyzer
 * @param sample_rate 48000 Hz
 * @param history_samples Number of samples to analyze (e.g., 1024)
 */
signal_analyzer_t *signal_analyzer_create(int sample_rate, int history_samples);

/**
 * Analyze signal and extract features
 * @return Extracted features
 */
signal_features_t signal_analyzer_process(signal_analyzer_t *analyzer,
                                          float complex *signal,
                                          int count);

/**
 * Get frequency estimate using phase tracking
 */
float signal_analyzer_get_frequency(signal_analyzer_t *analyzer, float center_freq);

/**
 * Destroy analyzer
 */
void signal_analyzer_destroy(signal_analyzer_t *analyzer);

#endif
