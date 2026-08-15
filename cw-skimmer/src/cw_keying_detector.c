#include "cw_keying_detector.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAG_BLOCK_SAMPLES     24
#define HIST_CAPACITY         32
#define MIN_MARK_SAMPLES      96
#define MIN_SPACE_SAMPLES     48
#define KEY_DEBOUNCE_BLOCKS   6
#define KEY_DEBOUNCE_WEAK     8
#define KEY_MIN_DIT_FRACTION  0.22f
#define KEY_MARK_HANG_RATIO   0.46f
#define KEY_MARK_FLOOR_RATIO  1.02f
#define KEY_MARK_MERGE_RATIO  0.38f

typedef struct {
    int durations[HIST_CAPACITY];
    int count;
    int head;
} duration_hist_t;

struct cw_keying_detector {
    cw_decoder_t *decoder;
    int sample_rate;
    float tone_hz;

    long long sample_index;

    float mix_lp_i;
    float mix_lp_q;
    int mix_lp_count;
    float magnitude_sum;

    float noise_floor;
    float smoothed_mag;
    float env_fast;
    float env_slow;
    float peak_snr;
    float mark_peak;

    int key_down;
    int run_samples;
    int pending_key;
    int pending_blocks;
    int open_mark_samples;
    char trace_buf[256];
    int trace_len;

    duration_hist_t mark_hist;
    duration_hist_t space_hist;

    float mean_dit;
    float mean_dah;
    float mean_char_space;
    float mean_word_space;
    int wpm_estimate;
};

static int hist_push(duration_hist_t *hist, int duration)
{
    if (!hist || duration <= 0) {
        return 0;
    }
    hist->durations[hist->head] = duration;
    hist->head = (hist->head + 1) % HIST_CAPACITY;
    if (hist->count < HIST_CAPACITY) {
        hist->count++;
    }
    return 1;
}

static int hist_compare_int(const void *a, const void *b)
{
    return (*(const int *)a - *(const int *)b);
}

static void hist_copy_sorted(const duration_hist_t *hist, int *out, int *out_n)
{
    int i;
    int n = 0;

    if (!hist || !out || !out_n) {
        return;
    }

    for (i = 0; i < hist->count; ++i) {
        int idx = (hist->head - 1 - i + HIST_CAPACITY) % HIST_CAPACITY;
        out[n++] = hist->durations[idx];
    }
    qsort(out, (size_t)n, sizeof(int), hist_compare_int);
    *out_n = n;
}

static void keying_recompute_timing(cw_keying_detector_t *kd)
{
    int marks[HIST_CAPACITY];
    int spaces[HIST_CAPACITY];
    int mark_n = 0;
    int space_n = 0;
    int i;
    int dit_cut;
    long long dit_sum = 0;
    int dit_count = 0;
    long long dah_sum = 0;
    int dah_count = 0;
    long long char_sum = 0;
    int char_count = 0;
    long long word_sum = 0;
    int word_count = 0;
    float dit;

    if (!kd) {
        return;
    }

    hist_copy_sorted(&kd->mark_hist, marks, &mark_n);
    hist_copy_sorted(&kd->space_hist, spaces, &space_n);

    if (mark_n > 0) {
        dit_cut = mark_n / 4;
        if (dit_cut < 0) {
            dit_cut = 0;
        }
        kd->mean_dit = (float)marks[dit_cut];

        dit_sum = 0;
        dit_count = 0;
        for (i = 0; i < mark_n; ++i) {
            if ((float)marks[i] <= kd->mean_dit * 1.35f) {
                dit_sum += marks[i];
                dit_count++;
            }
        }
        if (dit_count > 0) {
            kd->mean_dit = (float)dit_sum / (float)dit_count;
        }

        dit = kd->mean_dit;
        for (i = 0; i < mark_n; ++i) {
            if ((float)marks[i] >= dit * 2.0f) {
                dah_sum += marks[i];
                dah_count++;
            }
        }
        if (dah_count > 0) {
            kd->mean_dah = (float)dah_sum / (float)dah_count;
        } else if (dit > 0.0f) {
            kd->mean_dah = dit * 2.5f;
        }
    }

    if (space_n > 0 && kd->mean_dit > 1.0f) {
        float dit = kd->mean_dit;

        for (i = 0; i < space_n; ++i) {
            if ((float)spaces[i] >= dit * 1.6f && (float)spaces[i] < dit * 5.5f) {
                char_sum += spaces[i];
                char_count++;
            }
        }
        if (char_count > 0) {
            kd->mean_char_space = (float)char_sum / (float)char_count;
        } else {
            kd->mean_char_space = dit * 3.0f;
        }

        for (i = 0; i < space_n; ++i) {
            if ((float)spaces[i] >= dit * 5.0f) {
                word_sum += spaces[i];
                word_count++;
            }
        }
        if (word_count > 0) {
            kd->mean_word_space = (float)word_sum / (float)word_count;
        } else {
            kd->mean_word_space = dit * 7.0f;
        }
    }

    if (kd->mean_dit > 1.0f) {
        float dit_floor;
        int wpm_est;
        int wpm_max = 45;

        dit_floor = (1.2f / (float)kd->wpm_estimate) * (float)kd->sample_rate * 0.75f;
        if (kd->mean_dit < dit_floor) {
            kd->mean_dit = dit_floor;
        }
        if (kd->mean_dah < kd->mean_dit * 2.0f) {
            kd->mean_dah = kd->mean_dit * 2.8f;
        } else if (kd->mean_dah > kd->mean_dit * 5.0f) {
            kd->mean_dah = kd->mean_dit * 3.0f;
        }

        wpm_est = (int)(1.2f * (float)kd->sample_rate / kd->mean_dit + 0.5f);
        if (kd->wpm_estimate < 5) {
            kd->wpm_estimate = 5;
        }
        if (kd->mark_hist.count < 8) {
            wpm_max = 32;
        }
        if (wpm_est < 5) {
            wpm_est = 5;
        }
        if (wpm_est > wpm_max) {
            wpm_est = wpm_max;
        }
        kd->wpm_estimate = wpm_est;
        cw_decoder_update_wpm(kd->decoder, kd->wpm_estimate);
        cw_decoder_set_timing(kd->decoder, kd->mean_dit, kd->mean_char_space, kd->mean_word_space);
    }
}

static float bayesian_mark_element_prob(int duration, float mean, float sigma)
{
    float z;
    if (mean <= 1.0f || sigma <= 1.0f) {
        return 0.0f;
    }
    z = ((float)duration - mean) / sigma;
    return expf(-0.5f * z * z);
}

static char keying_classify_mark_bayesian(cw_keying_detector_t *kd, int duration)
{
    float dit_mean;
    float dah_mean;
    float dit_sigma;
    float dah_sigma;
    float p_dit;
    float p_dah;
    float prior_dit = 0.62f;
    float prior_dah = 0.38f;

    if (!kd || duration <= 0) {
        return '.';
    }

    dit_mean = kd->mean_dit;
    dah_mean = kd->mean_dah;
    if (dah_mean < dit_mean * 1.5f) {
        dah_mean = dit_mean * 2.5f;
    }

    dit_sigma = dit_mean * 0.55f;
    dah_sigma = dah_mean * 0.50f;
    if (dit_sigma < 8.0f) {
        dit_sigma = 8.0f;
    }
    if (dah_sigma < 12.0f) {
        dah_sigma = 12.0f;
    }

    p_dit = prior_dit * bayesian_mark_element_prob(duration, dit_mean, dit_sigma);
    p_dah = prior_dah * bayesian_mark_element_prob(duration, dah_mean, dah_sigma);

    if (duration >= (int)(dit_mean * 1.75f)) {
        if (duration <= (int)(dit_mean * 4.5f)) {
            kd->mean_dah = 0.85f * kd->mean_dah + 0.15f * (float)duration;
        }
        return '-';
    }
    if (duration <= (int)(dit_mean * 1.15f)) {
        kd->mean_dit = 0.85f * kd->mean_dit + 0.15f * (float)duration;
        return '.';
    }

    if (p_dah >= p_dit && duration >= (int)(dit_mean * 1.40f)) {
        if (duration <= (int)(dit_mean * 4.5f)) {
            kd->mean_dah = 0.85f * kd->mean_dah + 0.15f * (float)duration;
        }
        return '-';
    }

    kd->mean_dit = 0.85f * kd->mean_dit + 0.15f * (float)duration;
    return '.';
}

static void keying_handle_space_gap(cw_keying_detector_t *kd, int duration)
{
    float char_thr;
    float word_thr;

    if (!kd || duration <= 0) {
        return;
    }

    if (kd->mean_dit > 1.0f && duration < (int)(kd->mean_dit * 0.45f)) {
        return;
    }

    hist_push(&kd->space_hist, duration);
    keying_recompute_timing(kd);

    char_thr = kd->mean_char_space;
    word_thr = kd->mean_word_space;
    if (char_thr < kd->mean_dit * 2.0f) {
        char_thr = kd->mean_dit * 2.5f;
    }
    if (word_thr < kd->mean_dit * 7.0f) {
        word_thr = kd->mean_dit * 7.0f;
    }

    if ((float)duration >= word_thr) {
        if (kd->trace_len + 2 < (int)sizeof(kd->trace_buf)) {
            kd->trace_buf[kd->trace_len++] = '|';
            kd->trace_buf[kd->trace_len] = '\0';
        }
        cw_decoder_on_word_gap(kd->decoder);
    } else if ((float)duration >= char_thr) {
        if (kd->trace_len + 2 < (int)sizeof(kd->trace_buf)) {
            kd->trace_buf[kd->trace_len++] = ' ';
            kd->trace_buf[kd->trace_len] = '\0';
        }
        cw_decoder_on_char_gap(kd->decoder);
    }
}

static int keying_min_mark_samples(const cw_keying_detector_t *kd)
{
    int min_mark = MIN_MARK_SAMPLES;

    if (kd && kd->mean_dit > 1.0f) {
        int dit_min = (int)(kd->mean_dit * KEY_MIN_DIT_FRACTION);
        if (dit_min > min_mark) {
            min_mark = dit_min;
        }
    }
    return min_mark;
}

static void keying_finish_mark(cw_keying_detector_t *kd, int duration)
{
    char element;
    int min_mark;

    if (!kd) {
        return;
    }

    min_mark = keying_min_mark_samples(kd);
    if (duration < min_mark) {
        return;
    }

    if (kd->mean_dit > 1.0f && duration < (int)(kd->mean_dit * 0.40f)) {
        return;
    }

    hist_push(&kd->mark_hist, duration);
    keying_recompute_timing(kd);

    element = keying_classify_mark_bayesian(kd, duration);
    if (kd->trace_len + 1 < (int)sizeof(kd->trace_buf)) {
        kd->trace_buf[kd->trace_len++] = element;
        kd->trace_buf[kd->trace_len] = '\0';
    }
    cw_decoder_add_element(kd->decoder, element);
}

static float keying_finish_envelope_block(cw_keying_detector_t *kd)
{
    float mag = 0.0f;

    if (!kd || kd->mix_lp_count <= 0) {
        return 0.0f;
    }

    mag = kd->magnitude_sum / (float)kd->mix_lp_count;
    kd->mix_lp_i = 0.0f;
    kd->mix_lp_q = 0.0f;
    kd->mix_lp_count = 0;
    kd->magnitude_sum = 0.0f;

    return mag;
}

static void keying_push_mix_sample(cw_keying_detector_t *kd, float mix_i, float mix_q)
{
    const float alpha = 0.28f;
    float mag;

    kd->mix_lp_i = (1.0f - alpha) * kd->mix_lp_i + alpha * mix_i;
    kd->mix_lp_q = (1.0f - alpha) * kd->mix_lp_q + alpha * mix_q;
    kd->mix_lp_count++;

    mag = sqrtf(kd->mix_lp_i * kd->mix_lp_i + kd->mix_lp_q * kd->mix_lp_q);
    kd->magnitude_sum += mag;
}

static int keying_debounce_blocks(const cw_keying_detector_t *kd)
{
    if (!kd) {
        return KEY_DEBOUNCE_BLOCKS;
    }
    if (kd->peak_snr < 14.0f) {
        return KEY_DEBOUNCE_WEAK;
    }
    return KEY_DEBOUNCE_BLOCKS;
}

static int keying_threshold_key(cw_keying_detector_t *kd, float mag)
{
    float on_ratio;
    float off_thr;

    if (kd->env_slow <= 0.0f) {
        kd->env_slow = mag;
        kd->env_fast = mag;
        kd->noise_floor = mag;
        kd->mark_peak = mag;
    }

    kd->smoothed_mag = 0.82f * kd->smoothed_mag + 0.18f * mag;
    kd->env_fast = 0.55f * mag + 0.45f * kd->env_fast;
    if (!kd->key_down) {
        kd->env_slow = 0.06f * mag + 0.94f * kd->env_slow;
    } else if (mag > kd->mark_peak) {
        kd->mark_peak = mag;
    }

    if (kd->peak_snr < 8.0f) {
        on_ratio = 1.10f;
    } else if (kd->peak_snr < 12.0f) {
        on_ratio = 1.13f;
    } else if (kd->peak_snr < 18.0f) {
        on_ratio = 1.16f;
    } else {
        on_ratio = 1.19f;
    }

    if (kd->key_down) {
        float hang_thr = kd->mark_peak * KEY_MARK_HANG_RATIO;
        float noise_thr = kd->env_slow * KEY_MARK_FLOOR_RATIO;

        off_thr = hang_thr > noise_thr ? hang_thr : noise_thr;
        return (kd->env_fast >= off_thr) ? 1 : 0;
    }
    return (kd->env_fast > kd->env_slow * on_ratio) ? 1 : 0;
}

static void keying_transition(cw_keying_detector_t *kd, int new_key);

static void keying_apply_block(cw_keying_detector_t *kd, float mag)
{
    int raw_key;

    if (!kd) {
        return;
    }

    raw_key = keying_threshold_key(kd, mag);
    if (raw_key != kd->pending_key) {
        kd->pending_key = raw_key;
        kd->pending_blocks = 1;
        if (raw_key == kd->key_down) {
            kd->run_samples += MAG_BLOCK_SAMPLES;
        }
        return;
    }

    kd->pending_blocks++;
    if (kd->pending_blocks < keying_debounce_blocks(kd)) {
        if (raw_key == kd->key_down) {
            kd->run_samples += MAG_BLOCK_SAMPLES;
        }
        return;
    }

    keying_transition(kd, raw_key);
}

static int keying_merge_space_samples(const cw_keying_detector_t *kd, int space_samples)
{
    if (!kd || kd->mean_dit <= 1.0f) {
        return 0;
    }
    return space_samples < (int)(kd->mean_dit * KEY_MARK_MERGE_RATIO);
}

static void keying_transition(cw_keying_detector_t *kd, int new_key)
{
    if (!kd) {
        return;
    }

    if (new_key == kd->key_down) {
        kd->run_samples += MAG_BLOCK_SAMPLES;
        return;
    }

    if (kd->key_down) {
        kd->open_mark_samples += kd->run_samples;
    } else if (kd->run_samples >= MIN_SPACE_SAMPLES) {
        if (kd->open_mark_samples > 0) {
            if (keying_merge_space_samples(kd, kd->run_samples)) {
                kd->open_mark_samples += kd->run_samples;
            } else {
                keying_finish_mark(kd, kd->open_mark_samples);
                kd->open_mark_samples = 0;
                keying_handle_space_gap(kd, kd->run_samples);
            }
        } else {
            keying_handle_space_gap(kd, kd->run_samples);
        }
    } else if (kd->open_mark_samples > 0) {
        kd->open_mark_samples += kd->run_samples;
    }

    kd->key_down = new_key;
    kd->run_samples = MAG_BLOCK_SAMPLES;

    if (kd->key_down) {
        if (kd->open_mark_samples > 0) {
            kd->open_mark_samples += MAG_BLOCK_SAMPLES;
        }
        kd->mark_peak = kd->env_fast > kd->smoothed_mag ? kd->env_fast : kd->smoothed_mag;
    } else {
        kd->noise_floor = 0.97f * kd->noise_floor + 0.03f * kd->smoothed_mag;
        kd->mark_peak = 0.0f;
    }
}

cw_keying_detector_t *cw_keying_detector_create(cw_decoder_t *decoder,
                                               int sample_rate,
                                               float tone_hz,
                                               int initial_wpm)
{
    cw_keying_detector_t *kd;
    float dit_seconds;

    if (!decoder || sample_rate <= 0) {
        return NULL;
    }

    kd = calloc(1, sizeof(*kd));
    if (!kd) {
        return NULL;
    }

    kd->decoder = decoder;
    kd->sample_rate = sample_rate;
    kd->tone_hz = tone_hz;
    kd->peak_snr = 12.0f;

    if (initial_wpm < 5) {
        initial_wpm = 20;
    }
    dit_seconds = 1.2f / (float)initial_wpm;
    kd->mean_dit = dit_seconds * (float)sample_rate;
    kd->mean_dah = kd->mean_dit * 2.5f;
    kd->mean_char_space = kd->mean_dit * 2.5f;
    kd->mean_word_space = kd->mean_dit * 5.5f;
    kd->wpm_estimate = initial_wpm;
    kd->pending_key = 0;
    kd->pending_blocks = 0;

    cw_decoder_set_timing(decoder, kd->mean_dit, kd->mean_char_space, kd->mean_word_space);

    LOG_DEBUG("Keying detector: tone=%.1f Hz dit=%.0f samples", tone_hz, kd->mean_dit);
    return kd;
}

void cw_keying_detector_destroy(cw_keying_detector_t *kd)
{
    if (!kd) {
        return;
    }
    free(kd);
}

void cw_keying_detector_set_tone(cw_keying_detector_t *kd, float tone_hz)
{
    if (!kd || kd->sample_rate <= 0) {
        return;
    }
    kd->tone_hz = tone_hz;
}

void cw_keying_detector_set_snr(cw_keying_detector_t *kd, float snr_db)
{
    if (!kd) {
        return;
    }
    kd->peak_snr = snr_db;
}

void cw_keying_detector_flush(cw_keying_detector_t *kd)
{
    float mag;

    if (!kd) {
        return;
    }

    if (kd->mix_lp_count > 0) {
        mag = keying_finish_envelope_block(kd);
        keying_apply_block(kd, mag);
    }

    if (kd->open_mark_samples > 0) {
        kd->open_mark_samples += kd->run_samples;
        keying_finish_mark(kd, kd->open_mark_samples);
        kd->open_mark_samples = 0;
    } else if (kd->key_down) {
        keying_finish_mark(kd, kd->run_samples);
    } else if (kd->run_samples >= MIN_SPACE_SAMPLES) {
        keying_handle_space_gap(kd, kd->run_samples);
    }

    kd->key_down = 0;
    kd->run_samples = 0;
    kd->pending_key = 0;
    kd->pending_blocks = 0;
}

void cw_keying_detector_process_iq(cw_keying_detector_t *kd,
                                   const float complex *iq_samples,
                                   int count)
{
    int i;
    float omega;

    if (!kd || !iq_samples || count <= 0 || kd->sample_rate <= 0) {
        return;
    }

    omega = 2.0f * (float)M_PI * kd->tone_hz / (float)kd->sample_rate;

    for (i = 0; i < count; ++i) {
        float phase = omega * (float)kd->sample_index;
        float c = cosf(phase);
        float s = sinf(phase);
        float re = crealf(iq_samples[i]);
        float im = cimagf(iq_samples[i]);
        float mix_i = re * c + im * s;
        float mix_q = -re * s + im * c;
        float mag;

        keying_push_mix_sample(kd, mix_i, mix_q);
        kd->sample_index++;

        if (kd->mix_lp_count < MAG_BLOCK_SAMPLES) {
            continue;
        }

        mag = keying_finish_envelope_block(kd);
        keying_apply_block(kd, mag);
    }
}

float cw_keying_detector_get_mean_dit(const cw_keying_detector_t *kd)
{
    return kd ? kd->mean_dit : 0.0f;
}

float cw_keying_detector_get_mean_dah(const cw_keying_detector_t *kd)
{
    return kd ? kd->mean_dah : 0.0f;
}

int cw_keying_detector_get_wpm_estimate(const cw_keying_detector_t *kd)
{
    return kd ? kd->wpm_estimate : 20;
}

const char *cw_keying_detector_get_trace(const cw_keying_detector_t *kd)
{
    if (!kd || kd->trace_len <= 0) {
        return "";
    }
    return kd->trace_buf;
}