#include "signal_analyzer.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

signal_analyzer_t *signal_analyzer_create(int sample_rate, int history_samples) {
    signal_analyzer_t *analyzer = malloc(sizeof(signal_analyzer_t));
    if (!analyzer) return NULL;
    
    analyzer->signal_buffer = malloc(history_samples * sizeof(float complex));
    if (!analyzer->signal_buffer) {
        free(analyzer);
        return NULL;
    }
    
    analyzer->sample_rate = sample_rate;
    analyzer->buffer_size = history_samples;
    analyzer->buffer_index = 0;
    analyzer->mean_magnitude = 0.0;
    analyzer->variance = 0.0;
    analyzer->prev_magnitude = 0.0;
    
    memset(analyzer->signal_buffer, 0, history_samples * sizeof(float complex));
    
    LOG_DEBUG("Signal analyzer created: %d samples @ %d Hz",
              history_samples, sample_rate);
    
    return analyzer;
}

signal_features_t signal_analyzer_process(signal_analyzer_t *analyzer,
                                          float complex *signal,
                                          int count) {
    signal_features_t features;
    memset(&features, 0, sizeof(features));
    
    if (!analyzer || !signal || count <= 0) {
        return features;
    }
    
    // Add samples to buffer (circular)
    for (int i = 0; i < count && i < analyzer->buffer_size; i++) {
        analyzer->signal_buffer[analyzer->buffer_index] = signal[i];
        analyzer->buffer_index = (analyzer->buffer_index + 1) % analyzer->buffer_size;
    }
    
    // Compute statistics
    float sum_mag = 0.0;
    float sum_mag_sq = 0.0;
    float prev_mag = 0.0;
    float mag_variance = 0.0;
    float envelope_sharpness = 0.0;
    int transitions = 0;
    
    for (int i = 0; i < analyzer->buffer_size; i++) {
        float mag = cabsf(analyzer->signal_buffer[i]);
        sum_mag += mag;
        sum_mag_sq += mag * mag;
        
        // Envelope transitions (sharp edges indicate CW)
        if (i > 0) {
            float mag_change = fabsf(mag - prev_mag);
            if (mag_change > 0.5) {
                transitions++;
                envelope_sharpness += mag_change;
            }
        }
        prev_mag = mag;
    }
    
    analyzer->mean_magnitude = sum_mag / analyzer->buffer_size;
    mag_variance = (sum_mag_sq / analyzer->buffer_size) - 
                   (analyzer->mean_magnitude * analyzer->mean_magnitude);
    analyzer->variance = sqrtf(fmaxf(0.0, mag_variance));
    
    // Tone purity: low magnitude variance indicates pure sine
    float mag_cv = analyzer->variance / (analyzer->mean_magnitude + 1e-10);
    features.tone_purity = fmaxf(0.0, 1.0 - mag_cv * 5.0);  // 0.2 CV → purity=0
    
    // Envelope shape from transition sharpness
    features.envelope_shape = fminf(1.0, envelope_sharpness / 100.0);
    
    // Frequency stability via phase tracking
    float phase_drift = 0.0;
    for (int i = 1; i < analyzer->buffer_size; i++) {
        float complex s1 = analyzer->signal_buffer[i-1];
        float complex s2 = analyzer->signal_buffer[i];
        
        float phase1 = cargf(s1);
        float phase2 = cargf(s2);
        float phase_diff = phase2 - phase1;
        
        // Normalize to [-π, π]
        while (phase_diff > M_PI) phase_diff -= 2 * M_PI;
        while (phase_diff < -M_PI) phase_diff += 2 * M_PI;
        
        phase_drift += fabsf(phase_diff);
    }
    
    float avg_phase_drift = phase_drift / (analyzer->buffer_size - 1);
    features.freq_stability = fmaxf(0.0, 1.0 - avg_phase_drift / M_PI);  // Lower drift = higher stability
    
    // Keying regularity (for now, proxy to frequency stability)
    features.keying_regularity = features.freq_stability * 0.8 + 0.2;
    
    // Bandwidth estimation (width of frequency content)
    float bandwidth_estimate = mag_variance * analyzer->sample_rate / 48000.0;
    features.bandwidth_hz = bandwidth_estimate;
    
    // SNR (simple power-based estimate, needs noise floor)
    float signal_power = analyzer->mean_magnitude * analyzer->mean_magnitude;
    features.snr_db = 10.0 * log10f(fmaxf(signal_power, 1e-10) / 1e-5);
    
    return features;
}

float signal_analyzer_get_frequency(signal_analyzer_t *analyzer, float center_freq) {
    if (!analyzer) return center_freq;
    
    // Goertzel algorithm could be used for precise frequency estimation
    // For now, return center frequency
    
    // Phase coherence method
    float phase_accumulator = 0.0;
    for (int i = 1; i < analyzer->buffer_size; i++) {
        float complex s1 = analyzer->signal_buffer[i-1];
        float complex s2 = analyzer->signal_buffer[i];
        
        float phase_diff = cargf(s2 / s1);
        phase_accumulator += phase_diff;
    }
    
    float freq_offset = phase_accumulator * analyzer->sample_rate / 
                        (2 * M_PI * analyzer->buffer_size);
    
    return center_freq + freq_offset;
}

void signal_analyzer_destroy(signal_analyzer_t *analyzer) {
    if (!analyzer) return;
    
    if (analyzer->signal_buffer) {
        free(analyzer->signal_buffer);
    }
    
    free(analyzer);
    LOG_DEBUG("Signal analyzer destroyed");
}
