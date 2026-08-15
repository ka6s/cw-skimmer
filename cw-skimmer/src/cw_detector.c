#include "cw_detector.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SMOOTHING_FACTOR 0.1
#define NOISE_FLOOR_PERCENTILE 10
#define DETECTOR_FFT_BINS 1024

/* In-place radix-2 FFT (n must be power of 2). */
static void fft_radix2(float complex *x, int n)
{
    if (n <= 1) {
        return;
    }

    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j >= bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float complex tmp = x[i];
            x[i] = x[j];
            x[j] = tmp;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2.0f * M_PI / (float)len;
        float complex wlen = cosf(ang) + I * sinf(ang);
        for (int i = 0; i < n; i += len) {
            float complex w = 1.0f;
            for (int j = 0; j < len / 2; j++) {
                float complex u = x[i + j];
                float complex v = x[i + j + len / 2] * w;
                x[i + j] = u + v;
                x[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

static void map_spectrum_to_channels(cw_detector_t *detector,
                                     const float *power_db, int num_bins,
                                     int sample_rate)
{
    int nch = detector->num_channels;
    float bin_hz = (float)sample_rate / (float)num_bins;
    float ch_hz = (float)sample_rate / (float)nch;
    float chan_power[512];

    if (nch > 512 || num_bins <= 0) {
        return;
    }

    for (int ch = 0; ch < nch; ch++) {
        chan_power[ch] = -120.0f;
    }

    /* Single pass over FFT bins (O(num_bins)) instead of channels×bins. */
    for (int b = 0; b < num_bins; b++) {
        float bin_center = ((float)b - (float)num_bins * 0.5f + 0.5f) * bin_hz;
        int ch = (int)floorf((bin_center + (float)sample_rate * 0.5f) / ch_hz);
        if (ch < 0 || ch >= nch) {
            continue;
        }
        if (power_db[b] > chan_power[ch]) {
            chan_power[ch] = power_db[b];
        }
    }

    for (int ch = 0; ch < nch; ch++) {
        detector->smoothed_powers[ch] = (1.0f - SMOOTHING_FACTOR) * detector->smoothed_powers[ch] +
                                        SMOOTHING_FACTOR * chan_power[ch];
        detector->channel_powers[ch] = detector->smoothed_powers[ch];
    }
}

static int compute_fft_power_spectrum(const float complex *iq_samples, int count,
                                      int sample_rate, float *power_db, int fft_n)
{
    float complex fft_buf[DETECTOR_FFT_BINS];
    float complex dc;

    if (!iq_samples || !power_db || count <= 0 || fft_n <= 0 ||
        fft_n > DETECTOR_FFT_BINS || (fft_n & (fft_n - 1)) != 0) {
        return 0;
    }

    if (count < fft_n) {
        return 0;
    }

    dc = 0.0f;
    for (int i = 0; i < fft_n; i++) {
        int src_idx = count - fft_n + i;
        dc += iq_samples[src_idx];
    }
    dc /= (float)fft_n;

    for (int i = 0; i < fft_n; i++) {
        int src_idx = count - fft_n + i;
        float w = 0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(fft_n - 1)));
        fft_buf[i] = (iq_samples[src_idx] - dc) * w;
    }

    fft_radix2(fft_buf, fft_n);

    for (int i = 0; i < fft_n; i++) {
        int k = (i + fft_n / 2) % fft_n;
        float re = crealf(fft_buf[k]);
        float im = cimagf(fft_buf[k]);
        float mag2 = re * re + im * im;
        power_db[i] = 10.0f * log10f(fmaxf(mag2 / (float)fft_n, 1e-12f));
    }

    (void)sample_rate;
    return fft_n;
}

cw_detector_t *cw_detector_create(int sample_rate, int num_channels, int threshold) {
    cw_detector_t *detector = malloc(sizeof(cw_detector_t));
    if (!detector) return NULL;
    
    detector->classifier = bayesian_create(6);
    if (!detector->classifier) {
        free(detector);
        return NULL;
    }
    
    detector->channel_powers = malloc(num_channels * sizeof(float));
    detector->channel_frequencies = malloc(num_channels * sizeof(float));
    detector->smoothed_powers = malloc(num_channels * sizeof(float));
    
    if (!detector->channel_powers || !detector->channel_frequencies || !detector->smoothed_powers) {
        free(detector->channel_powers);
        free(detector->channel_frequencies);
        free(detector->smoothed_powers);
        bayesian_destroy(detector->classifier);
        free(detector);
        return NULL;
    }
    
    detector->num_channels = num_channels;
    detector->sample_rate = sample_rate;
    detector->detection_threshold = threshold;
    detector->min_snr_db = 3.0f;
    detector->hysteresis_count = 0;
    detector->min_detection_duration_ms = 50;
    detector->noise_floor = -80.0;
    
    memset(detector->channel_powers, 0, num_channels * sizeof(float));
    memset(detector->channel_frequencies, 0, num_channels * sizeof(float));
    memset(detector->smoothed_powers, 0, num_channels * sizeof(float));
    
    // Initialize channel center frequencies
    float channel_width = (float)sample_rate / num_channels;
    for (int i = 0; i < num_channels; i++) {
        detector->channel_frequencies[i] = -sample_rate/2.0f + (i + 0.5f) * channel_width;
    }
    
    LOG_INFO("CW Detector created: %d channels, %d Hz resolution", 
             num_channels, (int)channel_width);
    
    return detector;
}

static float estimate_noise_floor(cw_detector_t *detector) {
    // Estimate noise floor as lower percentile of power distribution
    float sorted[detector->num_channels];
    memcpy(sorted, detector->channel_powers, detector->num_channels * sizeof(float));
    
    // Simple bubble sort for percentile
    for (int i = 0; i < detector->num_channels - 1; i++) {
        for (int j = 0; j < detector->num_channels - i - 1; j++) {
            if (sorted[j] > sorted[j + 1]) {
                float tmp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = tmp;
            }
        }
    }
    
    int percentile_idx = detector->num_channels * NOISE_FLOOR_PERCENTILE / 100;
    return sorted[percentile_idx];
}

int cw_detector_feed_spectrum(cw_detector_t *detector, const float *power_db,
                              int num_bins, int sample_rate)
{
    if (!detector || !power_db || num_bins <= 0) {
        return 0;
    }

    map_spectrum_to_channels(detector, power_db, num_bins, sample_rate);
    detector->noise_floor = estimate_noise_floor(detector);

    return 0;
}

int cw_detector_analyze(cw_detector_t *detector, float complex *iq_samples, int count) {
    float power_db[DETECTOR_FFT_BINS];

    if (!detector || !iq_samples || count <= 0) {
        return 0;
    }

    if (compute_fft_power_spectrum(iq_samples, count, detector->sample_rate,
                                 power_db, DETECTOR_FFT_BINS) <= 0) {
        return 0;
    }

    return cw_detector_feed_spectrum(detector, power_db, DETECTOR_FFT_BINS,
                                     detector->sample_rate);
}

int cw_detector_get_signals(cw_detector_t *detector, cw_signal_t *signals, int max_signals) {
    if (!detector || !signals) return 0;
    
    int signal_count = 0;
    float min_snr_db = detector->min_snr_db > 0.0f ? detector->min_snr_db : 3.0f;
    float threshold_db = detector->noise_floor + min_snr_db;
    float conf_required = detector->detection_threshold / 100.0f;

    for (int i = 0; i < detector->num_channels && signal_count < max_signals; i++) {
        float snr = detector->channel_powers[i] - detector->noise_floor;

        if (detector->channel_powers[i] <= threshold_db) {
            continue;
        }

        /* Local peak: must stand above neighbors */
        if (i > 0 && detector->channel_powers[i] <= detector->channel_powers[i - 1]) {
            continue;
        }
        if (i < detector->num_channels - 1 &&
            detector->channel_powers[i] < detector->channel_powers[i + 1]) {
            continue;
        }

        {
            float features[6];
            float confidence;
            float req = conf_required;

            features[0] = fminf(1.0f, snr / 20.0f);
            features[1] = 0.8f;
            features[2] = fminf(1.0f, (snr + 20.0f) / 80.0f);
            features[3] = 0.9f;
            features[4] = 0.85f;
            features[5] = 0.95f;

            confidence = bayesian_evaluate(detector->classifier, features);

            if (snr < 6.0f) {
                req *= 0.25f;
            } else if (snr < 10.0f) {
                req *= 0.40f;
            } else if (snr < 15.0f) {
                req *= 0.55f;
            }

            if (confidence >= req || snr >= min_snr_db + 1.0f) {
                signals[signal_count].frequency = detector->channel_frequencies[i];
                signals[signal_count].snr_db = snr;
                signals[signal_count].confidence = confidence;
                signals[signal_count].tone_purity = features[0];
                signals[signal_count].bandwidth = 50.0f;
                signals[signal_count].valid = 1;
                signal_count++;
            }
        }
    }
    
    return signal_count;
}

cw_signal_t cw_detector_get_channel_signal(cw_detector_t *detector, int channel) {
    cw_signal_t signal;
    memset(&signal, 0, sizeof(signal));
    
    if (!detector || channel < 0 || channel >= detector->num_channels) {
        return signal;
    }
    
    signal.frequency = detector->channel_frequencies[channel];
    signal.snr_db = detector->channel_powers[channel] - detector->noise_floor;
    signal.valid = (signal.snr_db > 3.0);
    
    return signal;
}

float cw_detector_get_noise_floor(cw_detector_t *detector) {
    return detector ? detector->noise_floor : -100.0;
}

void cw_detector_reset(cw_detector_t *detector) {
    if (!detector) return;
    memset(detector->channel_powers, 0, detector->num_channels * sizeof(float));
    memset(detector->smoothed_powers, 0, detector->num_channels * sizeof(float));
    LOG_DEBUG("CW detector reset");
}

void cw_detector_destroy(cw_detector_t *detector) {
    if (!detector) return;
    
    free(detector->channel_powers);
    free(detector->channel_frequencies);
    free(detector->smoothed_powers);
    bayesian_destroy(detector->classifier);
    free(detector);
    
    LOG_DEBUG("CW detector destroyed");
}