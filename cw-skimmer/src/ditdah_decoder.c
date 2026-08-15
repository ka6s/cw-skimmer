#include "ditdah_decoder.h"
#include "logger.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FREQ_MIN_HZ           200.0f
#define FREQ_MAX_HZ           1200.0f
#define DIT_DAH_BOUNDARY      2.0f
#define LETTER_SPACE_BOUNDARY 2.0f
#define WORD_SPACE_BOUNDARY   5.0f

#define MAX_AUDIO_SAMPLES     (DITDAH_TARGET_SAMPLE_RATE * 12)
#define MAX_POWER_SAMPLES     8192
#define MAX_INTERVALS         512
#define DECIMATE_ACCUM_MAX    8
#define BIRDIE_MIN_ACTIVE_SEC 4.0f
#define BIRDIE_MIN_ON_INTERVALS 3

typedef enum {
    BIQUAD_HIGHPASS,
    BIQUAD_LOWPASS
} biquad_type_t;

typedef struct {
    float a0, a1, a2, b1, b2;
    float x1, x2, y1, y2;
} biquad_t;

typedef struct {
    float coeff;
    float *window;
    int window_size;
} goertzel_t;

struct ditdah_decoder {
    int source_sample_rate;
    float tone_hz;
    time_t created_at;

    biquad_t filter_hp;
    biquad_t filter_lp;

    float *audio_buffer;
    int audio_len;
    int audio_cap;

    float decim_accum[DECIMATE_ACCUM_MAX];
    int decim_count;
    int decim_step;
    long long mix_index;

    char plain_text[256];
    char display_text[256];
    char pending[32];
    char last_emitted_plain[256];
    char new_letter;
    int dirty;

    float estimated_wpm;
    int keying_on_intervals;
    int has_keying;
    int birdie;
};

static void biquad_init(biquad_t *f, biquad_type_t type, float cutoff_hz, int sample_rate)
{
    float c = tanf((float)M_PI * cutoff_hz / (float)sample_rate);
    float sqrt2 = sqrtf(2.0f);
    float d;

    memset(f, 0, sizeof(*f));
    d = 1.0f / (1.0f + sqrt2 * c + c * c);

    if (type == BIQUAD_LOWPASS) {
        f->a0 = c * c * d;
        f->a1 = 2.0f * f->a0;
        f->a2 = f->a0;
        f->b1 = 2.0f * (c * c - 1.0f) * d;
        f->b2 = (1.0f - sqrt2 * c + c * c) * d;
    } else {
        f->a0 = d;
        f->a1 = -2.0f * d;
        f->a2 = d;
        f->b1 = 2.0f * (c * c - 1.0f) * d;
        f->b2 = (1.0f - sqrt2 * c + c * c) * d;
    }
}

static float biquad_process_sample(biquad_t *f, float x0)
{
    float y0 = f->a0 * x0 + f->a1 * f->x1 + f->a2 * f->x2
               - f->b1 * f->y1 - f->b2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x0;
    f->y2 = f->y1;
    f->y1 = y0;
    return y0;
}

static int compare_float(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    if (fa < fb) return -1;
    if (fa > fb) return 1;
    return 0;
}

static int compare_int(const void *a, const void *b)
{
    return (*(const int *)a - *(const int *)b);
}

static char morse_to_char(const char *s)
{
    if (!s) return '\0';
    if (strcmp(s, ".-") == 0) return 'A';
    if (strcmp(s, "-...") == 0) return 'B';
    if (strcmp(s, "-.-.") == 0) return 'C';
    if (strcmp(s, "-..") == 0) return 'D';
    if (strcmp(s, ".") == 0) return 'E';
    if (strcmp(s, "..-.") == 0) return 'F';
    if (strcmp(s, "--.") == 0) return 'G';
    if (strcmp(s, "....") == 0) return 'H';
    if (strcmp(s, "..") == 0) return 'I';
    if (strcmp(s, ".---") == 0) return 'J';
    if (strcmp(s, "-.-") == 0) return 'K';
    if (strcmp(s, ".-..") == 0) return 'L';
    if (strcmp(s, "--") == 0) return 'M';
    if (strcmp(s, "-.") == 0) return 'N';
    if (strcmp(s, "---") == 0) return 'O';
    if (strcmp(s, ".--.") == 0) return 'P';
    if (strcmp(s, "--.-") == 0) return 'Q';
    if (strcmp(s, ".-.") == 0) return 'R';
    if (strcmp(s, "...") == 0) return 'S';
    if (strcmp(s, "-") == 0) return 'T';
    if (strcmp(s, "..-") == 0) return 'U';
    if (strcmp(s, "...-") == 0) return 'V';
    if (strcmp(s, ".--") == 0) return 'W';
    if (strcmp(s, "-..-") == 0) return 'X';
    if (strcmp(s, "-.--") == 0) return 'Y';
    if (strcmp(s, "--..") == 0) return 'Z';
    if (strcmp(s, ".----") == 0) return '1';
    if (strcmp(s, "..---") == 0) return '2';
    if (strcmp(s, "...--") == 0) return '3';
    if (strcmp(s, "....-") == 0) return '4';
    if (strcmp(s, ".....") == 0) return '5';
    if (strcmp(s, "-....") == 0) return '6';
    if (strcmp(s, "--...") == 0) return '7';
    if (strcmp(s, "---..") == 0) return '8';
    if (strcmp(s, "----.") == 0) return '9';
    if (strcmp(s, "-----") == 0) return '0';
    return '\0';
}

static void get_raw_intervals(const float *power_signal, int len, float threshold,
                              int *on, int *on_n, int *off, int *off_n)
{
    int current_len = 0;
    int is_on;
    int i;

    *on_n = 0;
    *off_n = 0;
    if (!power_signal || len <= 0) {
        return;
    }

    is_on = power_signal[0] > threshold;
    for (i = 0; i < len; ++i) {
        if ((power_signal[i] > threshold) == is_on) {
            current_len++;
        } else {
            if (is_on) {
                if (*on_n < MAX_INTERVALS) on[(*on_n)++] = current_len;
            } else {
                if (*off_n < MAX_INTERVALS) off[(*off_n)++] = current_len;
            }
            is_on = !is_on;
            current_len = 1;
        }
    }
    if (is_on) {
        if (*on_n < MAX_INTERVALS) on[(*on_n)++] = current_len;
    } else {
        if (*off_n < MAX_INTERVALS) off[(*off_n)++] = current_len;
    }
}

static void moving_average(const float *data, int len, int window_size,
                           float *out, int out_max, int *out_len)
{
    int i;
    float sum = 0.0f;
    int win_count = 0;

    *out_len = 0;
    if (!data || len <= 0 || !out || out_max <= 0) {
        return;
    }
    if (window_size <= 1) {
        int n = len < out_max ? len : out_max;
        memcpy(out, data, (size_t)n * sizeof(float));
        *out_len = n;
        return;
    }

    for (i = 0; i < len && *out_len < out_max; ++i) {
        sum += data[i];
        win_count++;
        if (win_count > window_size) {
            sum -= data[i - window_size];
            win_count = window_size;
        }
        out[(*out_len)++] = sum / (float)win_count;
    }
}

static int goertzel_init(goertzel_t *g, float target_freq, int sample_rate, int window_size)
{
    int i;
    float k = 0.5f + ((float)window_size * target_freq) / (float)sample_rate;
    float omega = (2.0f * (float)M_PI * k) / (float)window_size;

    g->window_size = window_size;
    g->coeff = 2.0f * cosf(omega);
    g->window = malloc((size_t)window_size * sizeof(float));
    if (!g->window) {
        return -1;
    }
    for (i = 0; i < window_size; ++i) {
        g->window[i] = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)window_size);
    }
    return 0;
}

static void goertzel_free(goertzel_t *g)
{
    free(g->window);
    g->window = NULL;
    g->window_size = 0;
}

static float goertzel_run(const goertzel_t *g, const float *samples)
{
    float q1 = 0.0f;
    float q2 = 0.0f;
    int i;

    for (i = 0; i < g->window_size; ++i) {
        float q0 = g->coeff * q1 - q2 + samples[i] * g->window[i];
        q2 = q1;
        q1 = q0;
    }
    return q1 * q1 + q2 * q2 - g->coeff * q1 * q2;
}

static int goertzel_process_decimated(const goertzel_t *g, const float *samples, int len,
                                      int step_size, float *out, int out_max)
{
    int out_len = 0;
    int i;

    if (!g || !samples || !out || len < g->window_size || step_size <= 0) {
        return 0;
    }
    for (i = 0; i + g->window_size <= len && out_len < out_max; i += step_size) {
        out[out_len++] = goertzel_run(g, samples + i);
    }
    return out_len;
}

static float calculate_cost(const float *power_signal, int len, float wpm,
                            float threshold, float power_signal_rate)
{
    int on[MAX_INTERVALS];
    int off[MAX_INTERVALS];
    int on_n = 0;
    int off_n = 0;
    float dot_len_samples;
    float on_norm[MAX_INTERVALS];
    float off_norm[MAX_INTERVALS];
    float short_elements[MAX_INTERVALS * 2];
    int short_n = 0;
    float median_dot_len;
    float cost_on = 0.0f;
    float cost_off = 0.0f;
    int i;

    get_raw_intervals(power_signal, len, threshold, on, &on_n, off, &off_n);
    if (on_n < 3 || off_n < 3) {
        return 1e30f;
    }

    dot_len_samples = (1200.0f / wpm / 1000.0f) * power_signal_rate;
    if (dot_len_samples < 1.0f) {
        return 1e30f;
    }

    for (i = 0; i < on_n; ++i) on_norm[i] = (float)on[i] / dot_len_samples;
    for (i = 0; i < off_n; ++i) off_norm[i] = (float)off[i] / dot_len_samples;

    for (i = 0; i < on_n; ++i) {
        if (on_norm[i] < 2.0f) short_elements[short_n++] = on_norm[i];
    }
    for (i = 0; i < off_n; ++i) {
        if (off_norm[i] < 2.0f) short_elements[short_n++] = off_norm[i];
    }
    if (short_n <= 0) {
        return 1e30f;
    }

    qsort(short_elements, (size_t)short_n, sizeof(float), compare_float);
    median_dot_len = short_elements[short_n / 2];
    if (median_dot_len < 0.25f) {
        return 1e30f;
    }

    for (i = 0; i < on_n; ++i) {
        float ratio = on_norm[i] / median_dot_len;
        float c1 = (ratio - 1.0f) * (ratio - 1.0f);
        float c3 = (ratio - 3.0f) * (ratio - 3.0f);
        cost_on += c1 < c3 ? c1 : c3;
    }
    for (i = 0; i < off_n; ++i) {
        float ratio = off_norm[i] / median_dot_len;
        float c1 = (ratio - 1.0f) * (ratio - 1.0f);
        float c3 = (ratio - 3.0f) * (ratio - 3.0f);
        float c7 = (ratio - 7.0f) * (ratio - 7.0f);
        float c = c1;
        if (c3 < c) c = c3;
        if (c7 < c) c = c7;
        cost_off += c;
    }

    return (cost_on / (float)on_n) + (cost_off / (float)off_n);
}

static int find_best_params(const float *power_signal, int len, float power_signal_rate,
                            float *best_wpm, float *best_threshold)
{
    float sorted[MAX_POWER_SAMPLES];
    int sorted_n = 0;
    float p25, p75, iqr;
    float threshold_candidates[3];
    float best_cost = 1e30f;
    int wpm_int;
    int ti;
    int i;

    if (!power_signal || len <= 0 || !best_wpm || !best_threshold) {
        return 0;
    }

    for (i = 0; i < len && sorted_n < MAX_POWER_SAMPLES; ++i) {
        if (power_signal[i] > 0.0f) {
            sorted[sorted_n++] = power_signal[i];
        }
    }
    if (sorted_n < 10) {
        return 0;
    }

    qsort(sorted, (size_t)sorted_n, sizeof(float), compare_float);
    p25 = sorted[(int)((float)sorted_n * 0.25f)];
    p75 = sorted[(int)((float)sorted_n * 0.75f)];
    iqr = p75 - p25;
    threshold_candidates[0] = p25 + iqr * 0.25f;
    threshold_candidates[1] = p25 + iqr * 0.50f;
    threshold_candidates[2] = p25 + iqr * 0.75f;

    *best_wpm = 20.0f;
    *best_threshold = threshold_candidates[1];

    for (ti = 0; ti < 3; ++ti) {
        for (wpm_int = 5; wpm_int <= 40; ++wpm_int) {
            float cost = calculate_cost(power_signal, len, (float)wpm_int,
                                        threshold_candidates[ti], power_signal_rate);
            if (cost < best_cost) {
                best_cost = cost;
                *best_wpm = (float)wpm_int;
                *best_threshold = threshold_candidates[ti];
            }
        }
    }
    return 1;
}

static int decode_with_params(const float *power_signal, int len, float wpm, float threshold,
                              char *out, int out_max, char *pending_out, int pending_max)
{
    int on[MAX_INTERVALS];
    int off[MAX_INTERVALS];
    int on_n = 0;
    int off_n = 0;
    int sorted_lengths[MAX_INTERVALS];
    float min_len, max_len, length_ratio, actual_dot_len;
    int debounce_samples;
    int current_len = 0;
    int is_on;
    int i;
    char current_letter[16];
    int letter_len = 0;
    int out_len = 0;

    (void)wpm;
    if (!power_signal || len <= 0 || !out || out_max <= 0) {
        if (pending_out && pending_max > 0) pending_out[0] = '\0';
        return 0;
    }

    out[0] = '\0';
    if (pending_out && pending_max > 0) pending_out[0] = '\0';

    get_raw_intervals(power_signal, len, threshold, on, &on_n, off, &off_n);
    if (on_n <= 0) {
        return 0;
    }

    for (i = 0; i < on_n; ++i) sorted_lengths[i] = on[i];
    qsort(sorted_lengths, (size_t)on_n, sizeof(int), compare_int);

    min_len = (float)sorted_lengths[0];
    max_len = (float)sorted_lengths[on_n - 1];
    length_ratio = max_len / (min_len > 0.0f ? min_len : 1.0f);

    if (length_ratio > 2.0f) {
        int half = on_n / 2;
        actual_dot_len = (float)sorted_lengths[half / 2 > 0 ? half / 2 : 0];
    } else {
        float median_len = (float)sorted_lengths[on_n / 2];
        const float breakpoint = 18.0f;
        actual_dot_len = median_len > breakpoint ? median_len / 3.0f : median_len;
    }
    if (actual_dot_len < 1.0f) {
        actual_dot_len = 1.0f;
    }

    debounce_samples = (int)lroundf(actual_dot_len * 0.3f);
    if (debounce_samples < 1) debounce_samples = 1;

    is_on = power_signal[0] > threshold;
    current_len = 0;

    for (i = 0; i <= len; ++i) {
        float p = (i < len) ? power_signal[i] : 0.0f;
        if ((p > threshold) == is_on) {
            current_len++;
        } else {
            if (current_len > debounce_samples) {
                float len_norm = (float)current_len / actual_dot_len;
                if (is_on) {
                    if (letter_len + 1 < (int)sizeof(current_letter)) {
                        current_letter[letter_len++] = (len_norm < DIT_DAH_BOUNDARY) ? '.' : '-';
                        current_letter[letter_len] = '\0';
                    }
                } else if (len_norm > LETTER_SPACE_BOUNDARY) {
                    char c;
                    if (letter_len > 0) {
                        current_letter[letter_len] = '\0';
                        c = morse_to_char(current_letter);
                        if (out_len + 1 < out_max) {
                            out[out_len++] = c ? c : '?';
                        }
                        letter_len = 0;
                        current_letter[0] = '\0';
                    }
                    if (len_norm > WORD_SPACE_BOUNDARY) {
                        if (out_len > 0 && out[out_len - 1] != ' ' && out_len + 1 < out_max) {
                            out[out_len++] = ' ';
                        }
                    }
                }
            }
            is_on = !is_on;
            current_len = 1;
        }
    }

    if (letter_len > 0 && pending_out && pending_max > 0) {
        strncpy(pending_out, current_letter, (size_t)(pending_max - 1));
        pending_out[pending_max - 1] = '\0';
    }

    if (letter_len > 0) {
        char c = morse_to_char(current_letter);
        if (out_len + 1 < out_max) {
            out[out_len++] = c ? c : '?';
        }
    }

    out[out_len] = '\0';
    return out_len > 0;
}

static int ditdah_append_audio(ditdah_decoder_t *d, float sample)
{
    if (!d) return -1;

    if (d->audio_len >= d->audio_cap) {
        int drop = DITDAH_TARGET_SAMPLE_RATE / 2;
        if (drop >= d->audio_len) {
            d->audio_len = 0;
        } else {
            memmove(d->audio_buffer, d->audio_buffer + drop,
                    (size_t)(d->audio_len - drop) * sizeof(float));
            d->audio_len -= drop;
        }
    }

    if (d->audio_len >= d->audio_cap) {
        return -1;
    }

    d->audio_buffer[d->audio_len++] = sample;
    return 0;
}

static void ditdah_push_resampled(ditdah_decoder_t *d, float sample)
{
    float filtered;

    if (d->decim_step > 1) {
        d->decim_accum[d->decim_count++] = sample;
        if (d->decim_count < d->decim_step) {
            return;
        }
        sample = d->decim_accum[0];
        d->decim_count = 0;
    }

    filtered = biquad_process_sample(&d->filter_hp, sample);
    filtered = biquad_process_sample(&d->filter_lp, filtered);
    ditdah_append_audio(d, filtered);
}

static int ditdah_run_pipeline(ditdah_decoder_t *d)
{
    goertzel_t gz;
    float raw_power[MAX_POWER_SAMPLES];
    float smoothed[MAX_POWER_SAMPLES];
    int raw_len;
    int smooth_len;
    int goertzel_window;
    int step_size;
    float power_signal_rate;
    float best_wpm;
    float best_threshold;
    char decoded[256];
    char pending[32];
    int on[MAX_INTERVALS];
    int off[MAX_INTERVALS];
    int on_n = 0;
    int off_n = 0;
    int smooth_window;
    float pitch;

    if (!d || d->audio_len < DITDAH_TARGET_SAMPLE_RATE / 2) {
        return 0;
    }

    pitch = d->tone_hz;
    if (pitch < FREQ_MIN_HZ) pitch = FREQ_MIN_HZ;
    if (pitch > FREQ_MAX_HZ) pitch = FREQ_MAX_HZ;

    goertzel_window = (int)(DITDAH_TARGET_SAMPLE_RATE * 0.025f);
    if (goertzel_window < 32) goertzel_window = 32;
    step_size = goertzel_window / 4;
    if (step_size < 1) step_size = 1;

    if (goertzel_init(&gz, pitch, DITDAH_TARGET_SAMPLE_RATE, goertzel_window) != 0) {
        return 0;
    }

    raw_len = goertzel_process_decimated(&gz, d->audio_buffer, d->audio_len,
                                         step_size, raw_power, MAX_POWER_SAMPLES);
    goertzel_free(&gz);
    if (raw_len <= 0) {
        return 0;
    }

    power_signal_rate = (float)DITDAH_TARGET_SAMPLE_RATE / (float)step_size;
    smooth_window = (int)lroundf(power_signal_rate * 0.02f);
    if (smooth_window < 1) smooth_window = 1;

    moving_average(raw_power, raw_len, smooth_window, smoothed, MAX_POWER_SAMPLES, &smooth_len);
    if (smooth_len <= 0) {
        return 0;
    }

    if (!find_best_params(smoothed, smooth_len, power_signal_rate,
                          &best_wpm, &best_threshold)) {
        return 0;
    }

    d->estimated_wpm = best_wpm;
    get_raw_intervals(smoothed, smooth_len, best_threshold, on, &on_n, off, &off_n);
    d->keying_on_intervals = on_n;
    d->has_keying = (on_n >= BIRDIE_MIN_ON_INTERVALS);

    decoded[0] = '\0';
    pending[0] = '\0';
    decode_with_params(smoothed, smooth_len, best_wpm, best_threshold,
                       decoded, (int)sizeof(decoded), pending, (int)sizeof(pending));

    if (decoded[0] != '\0' && strcmp(decoded, d->plain_text) != 0) {
        strncpy(d->plain_text, decoded, sizeof(d->plain_text) - 1);
        d->plain_text[sizeof(d->plain_text) - 1] = '\0';
        strncpy(d->display_text, decoded, sizeof(d->display_text) - 1);
        d->display_text[sizeof(d->display_text) - 1] = '\0';
        strncpy(d->pending, pending, sizeof(d->pending) - 1);
        d->pending[sizeof(d->pending) - 1] = '\0';
        d->dirty = 1;
        return 1;
    }

    if (pending[0] != '\0' && strcmp(pending, d->pending) != 0) {
        strncpy(d->pending, pending, sizeof(d->pending) - 1);
        d->pending[sizeof(d->pending) - 1] = '\0';
        d->dirty = 1;
        return 1;
    }

    return 0;
}

ditdah_decoder_t *ditdah_decoder_create(int source_sample_rate, float tone_hz)
{
    ditdah_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }

    d->source_sample_rate = source_sample_rate > 0 ? source_sample_rate : 48000;
    d->tone_hz = tone_hz;
    d->created_at = time(NULL);
    d->estimated_wpm = 20.0f;
    d->audio_cap = MAX_AUDIO_SAMPLES;
    d->audio_buffer = malloc((size_t)d->audio_cap * sizeof(float));
    if (!d->audio_buffer) {
        free(d);
        return NULL;
    }

    if (d->source_sample_rate % DITDAH_TARGET_SAMPLE_RATE == 0) {
        d->decim_step = d->source_sample_rate / DITDAH_TARGET_SAMPLE_RATE;
    } else {
        d->decim_step = 1;
    }

    biquad_init(&d->filter_hp, BIQUAD_HIGHPASS, FREQ_MIN_HZ, DITDAH_TARGET_SAMPLE_RATE);
    biquad_init(&d->filter_lp, BIQUAD_LOWPASS, FREQ_MAX_HZ, DITDAH_TARGET_SAMPLE_RATE);

    LOG_DEBUG("ditdah decoder: src=%d Hz tone=%.1f decim=%d",
              d->source_sample_rate, d->tone_hz, d->decim_step);
    return d;
}

void ditdah_decoder_destroy(ditdah_decoder_t *decoder)
{
    if (!decoder) return;
    free(decoder->audio_buffer);
    free(decoder);
}

void ditdah_decoder_set_tone(ditdah_decoder_t *decoder, float tone_hz)
{
    if (!decoder) return;
    if (fabsf(decoder->tone_hz - tone_hz) > 2.0f) {
        decoder->tone_hz = tone_hz;
        ditdah_decoder_reset(decoder);
    } else {
        decoder->tone_hz = tone_hz;
    }
}

void ditdah_decoder_reset(ditdah_decoder_t *decoder)
{
    if (!decoder) return;
    decoder->audio_len = 0;
    decoder->decim_count = 0;
    decoder->mix_index = 0;
    decoder->plain_text[0] = '\0';
    decoder->display_text[0] = '\0';
    decoder->pending[0] = '\0';
    decoder->last_emitted_plain[0] = '\0';
    decoder->new_letter = '\0';
    decoder->dirty = 0;
    decoder->keying_on_intervals = 0;
    decoder->has_keying = 0;
    decoder->birdie = 0;
    decoder->created_at = time(NULL);
    biquad_init(&decoder->filter_hp, BIQUAD_HIGHPASS, FREQ_MIN_HZ, DITDAH_TARGET_SAMPLE_RATE);
    biquad_init(&decoder->filter_lp, BIQUAD_LOWPASS, FREQ_MAX_HZ, DITDAH_TARGET_SAMPLE_RATE);
}

void ditdah_decoder_feed_iq(ditdah_decoder_t *decoder,
                            const float complex *iq_samples,
                            int count)
{
    int i;
    float omega;

    if (!decoder || !iq_samples || count <= 0 || decoder->source_sample_rate <= 0) {
        return;
    }

    omega = 2.0f * (float)M_PI * decoder->tone_hz / (float)decoder->source_sample_rate;

    for (i = 0; i < count; ++i) {
        float phase = omega * (float)decoder->mix_index;
        float c = cosf(phase);
        float s = sinf(phase);
        float re = crealf(iq_samples[i]);
        float im = cimagf(iq_samples[i]);
        float audio = re * c + im * s;

        ditdah_push_resampled(decoder, audio);
        decoder->mix_index++;
    }
}

int ditdah_decoder_update(ditdah_decoder_t *decoder)
{
    int changed;
    float active_sec;

    if (!decoder) return 0;

    changed = ditdah_run_pipeline(decoder);

    active_sec = ditdah_decoder_active_seconds(decoder);
    if (active_sec >= BIRDIE_MIN_ACTIVE_SEC && !decoder->has_keying) {
        decoder->birdie = 1;
    }

    return changed;
}

void ditdah_decoder_flush(ditdah_decoder_t *decoder)
{
    if (!decoder) return;
    ditdah_run_pipeline(decoder);
}

const char *ditdah_decoder_get_text(const ditdah_decoder_t *decoder)
{
    return decoder ? decoder->display_text : "";
}

const char *ditdah_decoder_get_plain_text(const ditdah_decoder_t *decoder)
{
    return decoder ? decoder->plain_text : "";
}

const char *ditdah_decoder_get_pending(const ditdah_decoder_t *decoder)
{
    return decoder ? decoder->pending : "";
}

int ditdah_decoder_take_new_letter(ditdah_decoder_t *decoder, char *out)
{
    size_t old_len;
    size_t new_len;
    char c = '\0';

    if (!decoder || !out) return 0;

    old_len = strlen(decoder->last_emitted_plain);
    new_len = strlen(decoder->plain_text);

    if (new_len > old_len) {
        c = decoder->plain_text[old_len];
        if (c == ' ') {
            strncpy(decoder->last_emitted_plain, decoder->plain_text, sizeof(decoder->last_emitted_plain) - 1);
            decoder->last_emitted_plain[sizeof(decoder->last_emitted_plain) - 1] = '\0';
            if (new_len > old_len + 1) {
                c = decoder->plain_text[old_len + 1];
            } else {
                c = '\0';
            }
        }
        if (c != '\0') {
            decoder->new_letter = c;
            *out = c;
            strncpy(decoder->last_emitted_plain, decoder->plain_text, sizeof(decoder->last_emitted_plain) - 1);
            decoder->last_emitted_plain[sizeof(decoder->last_emitted_plain) - 1] = '\0';
            return 1;
        }
    }

    *out = '\0';
    return 0;
}

int ditdah_decoder_is_dirty(const ditdah_decoder_t *decoder)
{
    return decoder ? decoder->dirty : 0;
}

void ditdah_decoder_clear_dirty(ditdah_decoder_t *decoder)
{
    if (decoder) decoder->dirty = 0;
}

int ditdah_decoder_has_keying(const ditdah_decoder_t *decoder)
{
    return decoder ? decoder->has_keying : 0;
}

int ditdah_decoder_is_birdie(const ditdah_decoder_t *decoder)
{
    if (!decoder) return 0;
    if (decoder->birdie) return 1;
    if (ditdah_decoder_active_seconds(decoder) >= BIRDIE_MIN_ACTIVE_SEC &&
        !decoder->has_keying) {
        return 1;
    }
    return 0;
}

float ditdah_decoder_active_seconds(const ditdah_decoder_t *decoder)
{
    time_t now;
    if (!decoder) return 0.0f;
    now = time(NULL);
    if (now <= decoder->created_at) return 0.0f;
    return (float)(now - decoder->created_at);
}

int ditdah_decoder_get_wpm(const ditdah_decoder_t *decoder)
{
    if (!decoder) return 20;
    return (int)lroundf(decoder->estimated_wpm);
}