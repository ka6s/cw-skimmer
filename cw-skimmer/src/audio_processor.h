#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <complex.h>

/* Wide: full sample_rate span (default 48 kHz), 1024-bin FFT.
 * Narrow experiment: spectrum_span_hz=3000 → ±1.5 kHz around VFO, finer bins. */
#define AP_WIDE_FFT_N        1024
#define AP_NARROW_FFT_N      256
#define AP_NARROW_HOP        32    /* decimated samples between FFTs (~10.7 ms @ 3 kHz) */
#define AP_NARROW_RING       2048
#define AP_MAX_PENDING_SPEC  16
#define AP_MAX_FFT_N         AP_WIDE_FFT_N

typedef struct {
    // Polyphase filterbank parameters (legacy / unused for FFT path)
    float *filter_coefficients;
    int filter_length;
    int num_filters;
    
    // Delay line for filter history
    float complex *delay_line;
    int delay_index;
    
    // Output buffers (latest frame)
    float *power_spectrum;
    float complex *filtered_output;
    
    int sample_rate;
    int buffer_size;

    /* Spectrum mode */
    int spectrum_span_hz;   /* 0 or sample_rate = wide; 3000 = ±1.5 kHz experiment */
    int fft_n;              /* active FFT size */
    int effective_fs;       /* sample rate of stream fed to FFT */
    /* 1 = demod audio (real mono): single-sided 0..Nyquist spectrum, no ±IQ mirrors */
    int real_audio_mode;
    int spectrum_bins;      /* bins in last power spectrum (fft_n or fft_n/2 if real) */

    /* Narrowband decimator / hop FFT */
    float complex *narrow_ring;
    int narrow_ring_size;
    int narrow_w;
    int narrow_count;
    int narrow_decim;       /* e.g. 16 for 48k → 3k */
    int narrow_hop;
    int narrow_decim_phase;
    float complex lpf_state; /* 1-pole complex LPF state before decimate */
    float lpf_alpha;

    /* Queue of completed power spectra for multi-hop emit */
    float pending_spec[AP_MAX_PENDING_SPEC][AP_MAX_FFT_N];
    int pending_bins[AP_MAX_PENDING_SPEC];
    int pending_head;
    int pending_tail;
    int pending_count;
} audio_processor_t;

/**
 * Create audio processor
 * @param sample_rate 48000 Hz
 * @param num_channels FFT bins for wide mode (typically 1024)
 * @param filter_order Polyphase filter order (legacy)
 */
audio_processor_t *audio_processor_create(int sample_rate, int num_channels, int filter_order);

/**
 * Set spectrum span. span_hz <= 0 or >= sample_rate → full-band wide mode.
 * span_hz == 3000 → experimental ±1.5 kHz around VFO with finer bins / faster hop.
 * Safe to call while running (resets narrow buffers).
 */
int audio_processor_set_spectrum_span(audio_processor_t *processor, int span_hz);

int audio_processor_spectrum_span_hz(const audio_processor_t *processor);
int audio_processor_fft_n(const audio_processor_t *processor);
float audio_processor_bin_width(const audio_processor_t *processor);

/** deskHPSDR/audio_start: real mono baseband (single-sided spectrum). IQ: off. */
void audio_processor_set_real_audio_mode(audio_processor_t *processor, int enabled);
int audio_processor_real_audio_mode(const audio_processor_t *processor);
int audio_processor_spectrum_bins(const audio_processor_t *processor);

/**
 * Process I/Q samples. Produces zero or more spectrum frames into an internal queue.
 * @return Number of new spectrum frames ready to pop
 */
int audio_processor_process(audio_processor_t *processor, float complex *iq_input, int input_count);

/**
 * Pop one completed power spectrum (dB). Returns bins, or 0 if queue empty.
 */
int audio_processor_pop_spectrum(audio_processor_t *processor, float *power, int max_bins);

/**
 * Get power spectrum from last processing (latest frame only; may be stale if multi-hop).
 */
int audio_processor_get_power_spectrum(audio_processor_t *processor, float *power);

int audio_processor_find_peak(audio_processor_t *processor, int sample_rate,
                              float min_snr_db, float *offset_hz, float *snr_db);

int audio_processor_find_peaks(audio_processor_t *processor, int sample_rate,
                               float min_snr_db, float *offset_hz, float *snr_db,
                               int max_peaks);

int audio_processor_get_snr_summary(audio_processor_t *processor,
                                    float *avg_peak_snr_db,
                                    float *peak_snr_db,
                                    int *peak_count);

int audio_processor_get_bins(audio_processor_t *processor, float complex *output, int num_bins);

void audio_processor_reset(audio_processor_t *processor);

void audio_processor_destroy(audio_processor_t *processor);

#endif
