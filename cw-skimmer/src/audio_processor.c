#include "audio_processor.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Simple in-place radix-2 FFT for complex data (forward transform). n must be power of 2. */
static void fft_radix2(float complex *x, int n) {
    if (n <= 1) return;

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
        float ang = 2.0f * M_PI / len;
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

static void generate_hann_window(float *window, int length) {
    for (int i = 0; i < length; i++) {
        float x = 2.0f * M_PI * i / (length - 1);
        window[i] = 0.5 * (1.0 - cosf(x));
    }
}

static void generate_polyphase_filter(float *coeffs, int order, int num_phases) {
    float *temp = malloc(order * sizeof(float));
    if (!temp) return;
    float cutoff = 1.0f / num_phases;
    int mid = order / 2;
    for (int i = 0; i < order; i++) {
        int n = i - mid;
        if (n == 0) {
            temp[i] = 2.0f * cutoff;
        } else {
            temp[i] = sinf(2.0f * M_PI * cutoff * n) / (M_PI * n);
        }
    }
    float *window = malloc(order * sizeof(float));
    generate_hann_window(window, order);
    for (int i = 0; i < order; i++) {
        temp[i] *= window[i];
    }
    free(window);
    for (int i = 0; i < order; i++) {
        coeffs[i] = temp[i];
    }
    free(temp);
}

static void ap_push_spectrum(audio_processor_t *proc, const float *power, int n_bins)
{
    int slot;
    if (!proc || !power || n_bins <= 0 || n_bins > AP_MAX_FFT_N) {
        return;
    }
    if (proc->pending_count >= AP_MAX_PENDING_SPEC) {
        /* Drop oldest */
        proc->pending_head = (proc->pending_head + 1) % AP_MAX_PENDING_SPEC;
        proc->pending_count--;
    }
    slot = proc->pending_tail;
    memcpy(proc->pending_spec[slot], power, (size_t)n_bins * sizeof(float));
    proc->pending_bins[slot] = n_bins;
    proc->pending_tail = (proc->pending_tail + 1) % AP_MAX_PENDING_SPEC;
    proc->pending_count++;

    /* Keep "latest" mirror for get_power_spectrum / peaks */
    memcpy(proc->power_spectrum, power, (size_t)n_bins * sizeof(float));
    for (int i = n_bins; i < proc->num_filters; i++) {
        proc->power_spectrum[i] = -120.0f;
    }
}

/* Real-audio waterfall: CW pitch lives ~300–1200 Hz; radio filter is a few
 * hundred Hz wide. Showing full 0–24 kHz compresses that into a thin white
 * strip at the bottom. Crop to this span for display. */
#define AP_AUDIO_DISPLAY_MAX_HZ  3500.0f
#define AP_AUDIO_LF_CUT_HZ       120.0f

/*
 * Complex IQ: full fftshift spectrum, DC at n/2, ±Fs/2.
 * Real audio: mono, single-sided, crop to CW audio band, LF suppressed.
 * Returns number of bins written to power_out.
 */
static int ap_compute_fft_power(const float complex *iq, int n,
                                float *power_out, int notch_bins,
                                int real_audio_mode, int sample_rate_hz)
{
    float complex fft_buf[AP_MAX_FFT_N];
    float full[AP_MAX_FFT_N];
    float complex dc = 0.0f;
    int i;

    if (n <= 0 || n > AP_MAX_FFT_N || (n & (n - 1)) != 0) {
        return 0;
    }
    if (sample_rate_hz <= 0) {
        sample_rate_hz = 48000;
    }

    for (i = 0; i < n; i++) {
        if (real_audio_mode) {
            dc += crealf(iq[i]);
        } else {
            dc += iq[i];
        }
    }
    dc /= (float)n;

    for (i = 0; i < n; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(n - 1)));
        if (real_audio_mode) {
            float re = crealf(iq[i]) - crealf(dc);
            fft_buf[i] = re * w;
        } else {
            fft_buf[i] = (iq[i] - dc) * w;
        }
    }

    fft_radix2(fft_buf, n);

    if (real_audio_mode) {
        float bin_hz = (float)sample_rate_hz / (float)n;
        int full_side = n / 2;
        int max_bin = (int)(AP_AUDIO_DISPLAY_MAX_HZ / bin_hz + 0.5f);
        int lf_cut = (int)(AP_AUDIO_LF_CUT_HZ / bin_hz + 0.5f);
        float ref;
        int mid0, mid1, k;

        if (max_bin < 48) {
            max_bin = 48;
        }
        if (max_bin > full_side) {
            max_bin = full_side;
        }
        if (lf_cut < 2) {
            lf_cut = 2;
        }
        if (lf_cut > max_bin / 4) {
            lf_cut = max_bin / 4;
        }

        for (i = 0; i < max_bin; i++) {
            float re = crealf(fft_buf[i]);
            float im = cimagf(fft_buf[i]);
            float mag2 = re * re + im * im;
            if (i > 0) {
                mag2 *= 2.0f;
            }
            power_out[i] = 10.0f * log10f(fmaxf(mag2 / (float)n, 1e-12f));
        }

        /* Mid-band average used as LF floor / noise reference */
        mid0 = lf_cut + (max_bin - lf_cut) / 5;
        mid1 = mid0 + (max_bin - lf_cut) / 2;
        if (mid1 >= max_bin) {
            mid1 = max_bin - 1;
        }
        if (mid0 >= mid1) {
            mid0 = lf_cut;
            mid1 = max_bin - 1;
        }
        ref = 0.0f;
        k = 0;
        for (i = mid0; i <= mid1; i++) {
            ref += power_out[i];
            k++;
        }
        ref = (k > 0) ? (ref / (float)k) : -80.0f;

        /* Kill DC/rumble white strip at the bottom */
        for (i = 0; i < lf_cut; i++) {
            power_out[i] = ref - 3.0f;
        }
        (void)notch_bins;
        return max_bin;
    }

    /* Complex IQ: fftshift so DC is center */
    for (i = 0; i < n; i++) {
        int k = (i + n / 2) % n;
        float re = crealf(fft_buf[k]);
        float im = cimagf(fft_buf[k]);
        float mag2 = re * re + im * im;
        full[i] = 10.0f * log10f(fmaxf(mag2 / (float)n, 1e-12f));
    }
    memcpy(power_out, full, (size_t)n * sizeof(float));

    if (notch_bins > 0) {
        int dc_bin = n / 2;
        float ref = -120.0f;
        int left = dc_bin - notch_bins - 2;
        int right = dc_bin + notch_bins + 2;
        if (left >= 0) {
            ref = fmaxf(ref, power_out[left]);
        }
        if (right < n) {
            ref = fmaxf(ref, power_out[right]);
        }
        for (int d = -notch_bins; d <= notch_bins; d++) {
            int b = dc_bin + d;
            if (b >= 0 && b < n) {
                power_out[b] = ref;
            }
        }
    }
    return n;
}

static void ap_narrow_try_fft(audio_processor_t *proc)
{
    float complex frame[AP_NARROW_FFT_N];
    float power[AP_NARROW_FFT_N];
    int i;
    int start;
    int n;

    if (!proc->narrow_ring || proc->narrow_count < proc->fft_n) {
        return;
    }

    n = proc->fft_n;
    if (n <= 0 || n > AP_NARROW_FFT_N) {
        return;
    }

    /* Copy last fft_n samples from ring (oldest→newest) */
    start = (proc->narrow_w - n + proc->narrow_ring_size) % proc->narrow_ring_size;
    for (i = 0; i < n; i++) {
        frame[i] = proc->narrow_ring[(start + i) % proc->narrow_ring_size];
    }

    {
        int bins = ap_compute_fft_power(frame, n, power, 2, proc->real_audio_mode,
                                        proc->effective_fs > 0 ? proc->effective_fs
                                                               : proc->sample_rate);
        proc->spectrum_bins = bins;
        ap_push_spectrum(proc, power, bins);
    }
}

static int ap_process_wide(audio_processor_t *proc, float complex *iq_input, int input_count)
{
    float power[AP_WIDE_FFT_N];
    const int FFT_N = proc->fft_n;

    /* Keep legacy delay-line fill (unused for spectrum but preserved) */
    for (int sample_idx = 0; sample_idx < input_count; sample_idx++) {
        proc->delay_line[proc->delay_index] = iq_input[sample_idx];
        proc->delay_index = (proc->delay_index + 1) % proc->filter_length;
    }

    if (input_count < FFT_N || (FFT_N & (FFT_N - 1)) != 0 || FFT_N > AP_WIDE_FFT_N) {
        for (int i = 0; i < proc->num_filters; i++) {
            proc->power_spectrum[i] = -120.0f;
        }
        return 0;
    }

    {
        int bins = ap_compute_fft_power(iq_input + (input_count - FFT_N), FFT_N, power, 4,
                                        proc->real_audio_mode,
                                        proc->effective_fs > 0 ? proc->effective_fs
                                                               : proc->sample_rate);
        proc->spectrum_bins = bins;
        ap_push_spectrum(proc, power, bins);
    }
    return 1;
}

static int ap_process_narrow(audio_processor_t *proc, float complex *iq_input, int input_count)
{
    int produced = 0;
    int samples_since_fft = 0;
    int i;

    if (!proc->narrow_ring) {
        return 0;
    }

    /*
     * Complex baseband is already centered on VFO. One-pole LPF then decimate
     * to spectrum_span_hz complex samples/sec (covers ±span/2).
     */
    for (i = 0; i < input_count; i++) {
        float complex x = iq_input[i];
        proc->lpf_state += proc->lpf_alpha * (x - proc->lpf_state);

        proc->narrow_decim_phase++;
        if (proc->narrow_decim_phase < proc->narrow_decim) {
            continue;
        }
        proc->narrow_decim_phase = 0;

        proc->narrow_ring[proc->narrow_w] = proc->lpf_state;
        proc->narrow_w = (proc->narrow_w + 1) % proc->narrow_ring_size;
        if (proc->narrow_count < proc->narrow_ring_size) {
            proc->narrow_count++;
        }
        samples_since_fft++;

        if (proc->narrow_count >= proc->fft_n && samples_since_fft >= proc->narrow_hop) {
            ap_narrow_try_fft(proc);
            samples_since_fft = 0;
            produced++;
        }
    }

    return produced;
}

audio_processor_t *audio_processor_create(int sample_rate, int num_channels, int filter_order) {
    audio_processor_t *proc = calloc(1, sizeof(audio_processor_t));
    if (!proc) return NULL;
    
    proc->sample_rate = sample_rate;
    proc->num_filters = num_channels > 0 ? num_channels : AP_WIDE_FFT_N;
    if (proc->num_filters > AP_MAX_FFT_N) {
        proc->num_filters = AP_MAX_FFT_N;
    }
    proc->filter_length = filter_order > 0 ? filter_order : proc->num_filters;
    proc->buffer_size = proc->num_filters;
    proc->delay_index = 0;
    proc->spectrum_span_hz = sample_rate;
    proc->fft_n = AP_WIDE_FFT_N;
    proc->effective_fs = sample_rate;
    proc->real_audio_mode = 0;
    proc->spectrum_bins = AP_WIDE_FFT_N;
    
    proc->filter_coefficients = malloc((size_t)proc->filter_length * sizeof(float));
    if (!proc->filter_coefficients) {
        free(proc);
        return NULL;
    }
    generate_polyphase_filter(proc->filter_coefficients, proc->filter_length, proc->num_filters);
    
    proc->delay_line = calloc((size_t)proc->filter_length, sizeof(float complex));
    proc->power_spectrum = calloc((size_t)AP_MAX_FFT_N, sizeof(float));
    proc->filtered_output = calloc((size_t)proc->num_filters, sizeof(float complex));
    if (!proc->delay_line || !proc->power_spectrum || !proc->filtered_output) {
        audio_processor_destroy(proc);
        return NULL;
    }

    proc->narrow_ring = NULL;
    proc->narrow_ring_size = AP_NARROW_RING;
    proc->narrow_hop = AP_NARROW_HOP;
    
    LOG_INFO("Audio processor created: wide FFT %d bins @ %d Hz (bin=%.1f Hz)",
             proc->fft_n, sample_rate, (float)sample_rate / (float)proc->fft_n);
    
    return proc;
}

int audio_processor_set_spectrum_span(audio_processor_t *proc, int span_hz)
{
    if (!proc) {
        return -1;
    }

    if (span_hz <= 0 || span_hz >= proc->sample_rate) {
        /* Full-band wide mode (current production path) */
        proc->spectrum_span_hz = proc->sample_rate;
        proc->fft_n = AP_WIDE_FFT_N;
        proc->effective_fs = proc->sample_rate;
        proc->narrow_decim = 1;
        proc->pending_head = proc->pending_tail = proc->pending_count = 0;
        proc->narrow_w = 0;
        proc->narrow_count = 0;
        proc->narrow_decim_phase = 0;
        proc->lpf_state = 0;
        LOG_INFO("Spectrum mode WIDE: %d Hz span, FFT %d, bin=%.2f Hz",
                 proc->spectrum_span_hz, proc->fft_n,
                 (float)proc->effective_fs / (float)proc->fft_n);
        return 0;
    }

    /* Narrow experiment: complex Fs = span_hz covers ±span/2 */
    proc->spectrum_span_hz = span_hz;
    proc->fft_n = AP_NARROW_FFT_N;
    proc->effective_fs = span_hz;
    proc->narrow_decim = proc->sample_rate / span_hz;
    if (proc->narrow_decim < 2) {
        proc->narrow_decim = 2;
    }
    proc->narrow_hop = AP_NARROW_HOP;
    /* One-pole LPF ~ cutoff span/2 before decimation */
    proc->lpf_alpha = 1.0f - expf(-2.0f * (float)M_PI * (0.45f * (float)span_hz) /
                                  (float)proc->sample_rate);
    if (proc->lpf_alpha < 0.01f) {
        proc->lpf_alpha = 0.01f;
    }
    if (proc->lpf_alpha > 0.5f) {
        proc->lpf_alpha = 0.5f;
    }

    if (!proc->narrow_ring) {
        proc->narrow_ring = calloc((size_t)proc->narrow_ring_size, sizeof(float complex));
        if (!proc->narrow_ring) {
            return -1;
        }
    }
    memset(proc->narrow_ring, 0, (size_t)proc->narrow_ring_size * sizeof(float complex));
    proc->narrow_w = 0;
    proc->narrow_count = 0;
    proc->narrow_decim_phase = 0;
    proc->lpf_state = 0;
    proc->pending_head = proc->pending_tail = proc->pending_count = 0;

    LOG_INFO("Spectrum mode NARROW: %d Hz span (±%d Hz), decim=%d, FFT %d, hop %d, "
             "bin=%.2f Hz, column~%.1f ms",
             span_hz, span_hz / 2, proc->narrow_decim, proc->fft_n, proc->narrow_hop,
             (float)proc->effective_fs / (float)proc->fft_n,
             1000.0f * (float)proc->narrow_hop / (float)proc->effective_fs);
    return 0;
}

int audio_processor_spectrum_span_hz(const audio_processor_t *proc)
{
    return proc ? proc->spectrum_span_hz : 0;
}

int audio_processor_fft_n(const audio_processor_t *proc)
{
    return proc ? proc->fft_n : 0;
}

float audio_processor_bin_width(const audio_processor_t *proc)
{
    if (!proc || proc->fft_n <= 0) {
        return 0.0f;
    }
    return (float)proc->effective_fs / (float)proc->fft_n;
}

void audio_processor_set_real_audio_mode(audio_processor_t *proc, int enabled)
{
    if (!proc) {
        return;
    }
    if (proc->real_audio_mode == (enabled ? 1 : 0)) {
        return;
    }
    proc->real_audio_mode = enabled ? 1 : 0;
    proc->pending_head = proc->pending_tail = proc->pending_count = 0;
    LOG_INFO("Spectrum input mode: %s",
             proc->real_audio_mode
                 ? "REAL AUDIO (mono, single-sided 0..Nyquist — deskHPSDR/audio_start)"
                 : "COMPLEX IQ (full ±Fs/2 — Thetis/ExpertSDR iq_start)");
}

int audio_processor_real_audio_mode(const audio_processor_t *proc)
{
    return proc ? proc->real_audio_mode : 0;
}

int audio_processor_spectrum_bins(const audio_processor_t *proc)
{
    if (!proc) {
        return 0;
    }
    if (proc->spectrum_bins > 0) {
        return proc->spectrum_bins;
    }
    if (proc->real_audio_mode && proc->fft_n > 0) {
        return proc->fft_n / 2;
    }
    return proc->fft_n;
}

int audio_processor_process(audio_processor_t *proc, float complex *iq_input, int input_count) {
    if (!proc || !iq_input || input_count <= 0) return 0;

    if (proc->spectrum_span_hz > 0 && proc->spectrum_span_hz < proc->sample_rate) {
        return ap_process_narrow(proc, iq_input, input_count);
    }
    return ap_process_wide(proc, iq_input, input_count);
}

int audio_processor_pop_spectrum(audio_processor_t *proc, float *power, int max_bins)
{
    int slot;
    int n;
    if (!proc || !power || max_bins <= 0 || proc->pending_count <= 0) {
        return 0;
    }
    slot = proc->pending_head;
    n = proc->pending_bins[slot];
    if (n > max_bins) {
        n = max_bins;
    }
    memcpy(power, proc->pending_spec[slot], (size_t)n * sizeof(float));
    proc->pending_head = (proc->pending_head + 1) % AP_MAX_PENDING_SPEC;
    proc->pending_count--;
    return n;
}

int audio_processor_get_power_spectrum(audio_processor_t *proc, float *power) {
    int n;
    if (!proc || !power) return 0;
    n = audio_processor_spectrum_bins(proc);
    if (n <= 0) {
        n = proc->fft_n > 0 ? proc->fft_n : proc->num_filters;
    }
    memcpy(power, proc->power_spectrum, (size_t)n * sizeof(float));
    return n;
}

static float audio_estimate_noise_floor(const audio_processor_t *proc, int dc_bin, int dc_guard) {
    float sorted[AP_MAX_FFT_N];
    int n = audio_processor_spectrum_bins(proc);
    int count = 0;
    int i;

    if (n <= 0) {
        n = proc->fft_n > 0 ? proc->fft_n : proc->num_filters;
    }
    if (n > AP_MAX_FFT_N) {
        n = AP_MAX_FFT_N;
    }

    for (i = 0; i < n; i++) {
        if (i >= dc_bin - dc_guard && i <= dc_bin + dc_guard) {
            continue;
        }
        sorted[count++] = proc->power_spectrum[i];
    }

    if (count <= 0) {
        return -110.0f;
    }

    for (i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (sorted[j] > sorted[j + 1]) {
                float tmp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = tmp;
            }
        }
    }

    return sorted[count / 5];
}

int audio_processor_find_peak(audio_processor_t *proc, int sample_rate,
                              float min_snr_db, float *offset_hz, float *snr_db) {
    float offsets[1];
    float snrs[1];

    if (audio_processor_find_peaks(proc, sample_rate, min_snr_db,
                                   offsets, snrs, 1) > 0) {
        *offset_hz = offsets[0];
        *snr_db = snrs[0];
        return 1;
    }
    return 0;
}

int audio_processor_find_peaks(audio_processor_t *proc, int sample_rate,
                               float min_snr_db, float *offset_hz, float *snr_db,
                               int max_peaks) {
    int n;
    int dc;
    int guard;
    float noise;
    float bin_hz;
    int found;
    int i;
    int fs;

    if (!proc || !offset_hz || !snr_db || max_peaks <= 0) {
        return 0;
    }

    n = audio_processor_spectrum_bins(proc);
    if (n <= 0) {
        n = proc->fft_n > 0 ? proc->fft_n : proc->num_filters;
    }
    if (n <= 0) {
        return 0;
    }

    fs = proc->effective_fs > 0 ? proc->effective_fs : sample_rate;
    if (fs <= 0) {
        fs = proc->sample_rate;
    }

    /* Real audio: bins are 0..Nyquist; complex IQ: DC at n/2 */
    if (proc->real_audio_mode) {
        dc = 0;
        guard = 2;
        bin_hz = (float)fs / (float)proc->fft_n; /* full FFT length defines bin Hz */
    } else {
        dc = n / 2;
        guard = (n >= 512) ? 8 : 2;
        bin_hz = (float)fs / (float)n;
    }
    noise = audio_estimate_noise_floor(proc, dc, guard);
    found = 0;

    for (i = 1; i < n - 1 && found < max_peaks; i++) {
        float p = proc->power_spectrum[i];
        float snr;

        if (i >= dc - guard && i <= dc + guard) {
            continue;
        }
        if (p <= proc->power_spectrum[i - 1] || p < proc->power_spectrum[i + 1]) {
            continue;
        }

        snr = p - noise;
        if (snr < min_snr_db) {
            continue;
        }

        {
            float delta = 0.0f;
            float y1 = proc->power_spectrum[i - 1];
            float y3 = proc->power_spectrum[i + 1];
            float denom = y1 - 2.0f * p + y3;
            if (fabsf(denom) > 1e-6f) {
                delta = 0.5f * (y1 - y3) / denom;
                if (delta > 1.0f) {
                    delta = 1.0f;
                } else if (delta < -1.0f) {
                    delta = -1.0f;
                }
            }
            if (proc->real_audio_mode) {
                /* Audio Hz above 0 (CW pitch) */
                offset_hz[found] = ((float)i + delta) * bin_hz;
            } else {
                offset_hz[found] = ((float)i + delta - (float)n * 0.5f) * bin_hz;
            }
        }
        snr_db[found] = snr;
        found++;
    }

    return found;
}

int audio_processor_get_snr_summary(audio_processor_t *proc,
                                    float *avg_peak_snr_db,
                                    float *peak_snr_db,
                                    int *peak_count) {
    float peak_offsets[64];
    float peak_snrs[64];
    int n_peaks;
    int i;
    float sum;
    float peak;
    int fs;

    if (!proc || !avg_peak_snr_db || !peak_snr_db || !peak_count) {
        return -1;
    }

    fs = proc->effective_fs > 0 ? proc->effective_fs : proc->sample_rate;
    if (proc->fft_n <= 0 || fs <= 0) {
        *avg_peak_snr_db = 0.0f;
        *peak_snr_db = 0.0f;
        *peak_count = 0;
        return -1;
    }

    n_peaks = audio_processor_find_peaks(proc, fs, 0.0f,
                                         peak_offsets, peak_snrs, 64);
    if (n_peaks <= 0) {
        *avg_peak_snr_db = 0.0f;
        *peak_snr_db = 0.0f;
        *peak_count = 0;
        return 0;
    }

    sum = 0.0f;
    peak = peak_snrs[0];
    for (i = 0; i < n_peaks; i++) {
        sum += peak_snrs[i];
        if (peak_snrs[i] > peak) {
            peak = peak_snrs[i];
        }
    }

    *avg_peak_snr_db = sum / (float)n_peaks;
    *peak_snr_db = peak;
    *peak_count = n_peaks;
    return 0;
}

int audio_processor_get_bins(audio_processor_t *proc, float complex *output, int num_bins) {
    if (!proc || !output || num_bins <= 0) return 0;
    
    int to_copy = (num_bins < proc->num_filters) ? num_bins : proc->num_filters;
    memcpy(output, proc->filtered_output, to_copy * sizeof(float complex));
    
    return to_copy;
}

void audio_processor_reset(audio_processor_t *proc) {
    if (!proc) return;
    
    memset(proc->delay_line, 0, (size_t)proc->filter_length * sizeof(float complex));
    memset(proc->power_spectrum, 0, (size_t)AP_MAX_FFT_N * sizeof(float));
    memset(proc->filtered_output, 0, (size_t)proc->num_filters * sizeof(float complex));
    proc->delay_index = 0;
    proc->pending_head = proc->pending_tail = proc->pending_count = 0;
    proc->narrow_w = 0;
    proc->narrow_count = 0;
    proc->narrow_decim_phase = 0;
    proc->lpf_state = 0;
    if (proc->narrow_ring) {
        memset(proc->narrow_ring, 0, (size_t)proc->narrow_ring_size * sizeof(float complex));
    }
    
    LOG_DEBUG("Audio processor reset");
}

void audio_processor_destroy(audio_processor_t *proc) {
    if (!proc) return;
    
    free(proc->filter_coefficients);
    free(proc->delay_line);
    free(proc->power_spectrum);
    free(proc->filtered_output);
    free(proc->narrow_ring);
    free(proc);
    
    LOG_DEBUG("Audio processor destroyed");
}
