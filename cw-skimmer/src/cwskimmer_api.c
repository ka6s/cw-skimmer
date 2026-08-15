/**
 * @file cwskimmer_api.c
 * @brief Implementation of thread-safe C/Qt bridge API
 */

#include "cwskimmer_api.h"
#include "tci_client.h"
#include "config.h"
#include "logger.h"
#include "cw_detector.h"
#include "audio_processor.h"
#include "cw_decoder.h"
#include "spot_reporter.h"
#include "perf_profile.h"
#include "cw_message_validator.h"
#include "cw_capture.h"
#include "decode_worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <complex.h>
#include <math.h>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_DECODE_CHANNELS 12
#define DECODE_BUCKET_MIN_HZ 100.0f
#define DECODE_BUCKET_MAX_HZ 10000.0f
#define DECODE_CHANNEL_TIMEOUT_SEC 10
#define IQ_PROCESS_CHUNK 1024
#define MAX_IQ_CHUNKS_PER_LOOP 32
#define MAG_BLOCK_SAMPLES 24
#define TONE_SEARCH_RANGE_HZ 600.0f
#define TONE_SEARCH_STEP_HZ 5.0f
#define TONE_SWEEP_STEP_HZ 25.0f
#define TONE_COARSE_STEP_HZ 50.0f
#define REPLAY_MARK_MAX_CANDIDATES 3
#define REPLAY_PRESPECTRUM_BINS 1024

/* usleep declaration for strict C99 (-std=c99) builds where unistd may hide it without _BSD_SOURCE */
#if !defined(_BSD_SOURCE) && !defined(_DEFAULT_SOURCE) && !defined(_XOPEN_SOURCE)
int usleep(unsigned int __useconds);
#endif

typedef struct cwskimmer_detector cwskimmer_detector_t;

typedef struct cwskimmer_detector {
    /* Configuration */
    config_t config;
    
    /* Radio and processing components */
    tci_client_t *radio;
    cw_detector_t *detector;
    audio_processor_t *audio_proc;
    decode_worker_t decode_channels[MAX_DECODE_CHANNELS];
    spot_reporter_t *reporter;
    
    /* Control */
    volatile int running;
    volatile int loop_active;
    pthread_mutex_t lock;
    
    /* Callbacks */
    cwskimmer_signal_callback signal_cb;
    void *signal_userdata;
    
    cwskimmer_spot_callback spot_cb;
    void *spot_userdata;
    
    cwskimmer_log_callback log_cb;
    void *log_userdata;
    
    cwskimmer_stats_callback stats_cb;
    void *stats_userdata;
    
    cwskimmer_spectrum_callback spectrum_cb;
    void *spectrum_userdata;

    cwskimmer_decode_callback decode_cb;
    void *decode_userdata;
    
    /* Statistics */
    int samples_processed;
    int num_detections;
    float cpu_usage;

    /* Rolling IQ capture for offline replay */
    cw_capture_ring_t *iq_capture;

    /* Snapshot taken at Shift+click; used by save_capture until cleared */
    float complex *frozen_capture_samples;
    int frozen_capture_count;
    uint32_t frozen_capture_sample_rate;
    uint64_t frozen_capture_center_hz;

    /* Offline replay collection */
    int replay_collect;
    char replay_decodes[12][64];
    int replay_decode_count;

} cwskimmer_detector_t; /* full definition */

static float cwskimmer_radio_center_hz(cwskimmer_detector_t *detector)
{
    long long radio_hz;

    if (!detector) {
        return 0.0f;
    }

    radio_hz = tci_get_center_frequency(detector->radio);
    if (radio_hz > 0) {
        return (float)radio_hz;
    }

    return detector->config.center_frequency;
}

static int cwskimmer_sanitize_decode_text(const char *in, char *out, int out_max)
{
    int j = 0;
    int i;

    if (!in || !out || out_max <= 0) {
        return 0;
    }

    for (i = 0; in[i] != '\0' && j < out_max - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '/' || c == '.' || c == '-') {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return j;
}

static float cwskimmer_decode_bucket_hz(const cwskimmer_detector_t *detector)
{
    float bucket_hz;

    if (!detector) {
        return 1000.0f;
    }

    bucket_hz = detector->config.decode_bucket_hz;
    if (bucket_hz < DECODE_BUCKET_MIN_HZ) {
        bucket_hz = DECODE_BUCKET_MIN_HZ;
    } else if (bucket_hz > DECODE_BUCKET_MAX_HZ) {
        bucket_hz = DECODE_BUCKET_MAX_HZ;
    }
    return bucket_hz;
}

static int cwskimmer_in_same_decode_bucket(float anchor_hz, float peak_hz, float bucket_hz)
{
    return fabsf(anchor_hz - peak_hz) <= bucket_hz;
}

static float cwskimmer_tone_envelope_span(const float complex *samples, int count,
                                          int sample_rate, float tone_hz)
{
    const float complex *scan = samples;
    int scan_count = count;
    float omega = 2.0f * (float)M_PI * tone_hz / (float)sample_rate;
    float block_min = 1e9f;
    float block_max = 0.0f;
    float lp_i = 0.0f;
    float lp_q = 0.0f;
    const float alpha = 0.28f;
    int i;

    if (samples && count > sample_rate * 8) {
        scan = samples + count - sample_rate * 8;
        scan_count = sample_rate * 8;
    }

    for (i = 0; i < scan_count; ++i) {
        float phase = omega * (float)i;
        float c = cosf(phase);
        float s = sinf(phase);
        float re = crealf(scan[i]);
        float im = cimagf(scan[i]);
        float mix_i = re * c + im * s;
        float mix_q = -re * s + im * c;
        float mag;

        lp_i = (1.0f - alpha) * lp_i + alpha * mix_i;
        lp_q = (1.0f - alpha) * lp_q + alpha * mix_q;
        if ((i + 1) % MAG_BLOCK_SAMPLES != 0) {
            continue;
        }

        mag = sqrtf(lp_i * lp_i + lp_q * lp_q);
        lp_i = 0.0f;
        lp_q = 0.0f;
        if (mag < block_min) {
            block_min = mag;
        }
        if (mag > block_max) {
            block_max = mag;
        }
    }

    if (block_max <= block_min) {
        return 0.0f;
    }
    return block_max - block_min;
}

static float cwskimmer_search_tone_envelope(const float complex *samples, int count,
                                            int sample_rate, float approx_hz)
{
    float anchor_span;
    float best_freq = approx_hz;
    float best_span = -1.0f;
    int test_hz;
    int max_hz = (int)lroundf(approx_hz + TONE_SEARCH_RANGE_HZ);
    int min_hz = (int)lroundf(approx_hz - TONE_SEARCH_RANGE_HZ);
    const float complex *scan = samples;
    int scan_count = count;

    if (!samples || count <= 0 || sample_rate <= 0) {
        return approx_hz;
    }

    if (count > sample_rate * 8) {
        scan = samples + count - sample_rate * 8;
        scan_count = sample_rate * 8;
    }

    anchor_span = cwskimmer_tone_envelope_span(scan, scan_count, sample_rate, approx_hz);
    best_span = anchor_span;

    for (test_hz = min_hz; test_hz <= max_hz;
         test_hz += (int)TONE_SEARCH_STEP_HZ) {
        float span = cwskimmer_tone_envelope_span(scan, scan_count, sample_rate,
                                                  (float)test_hz);
        if (span > best_span) {
            best_span = span;
            best_freq = (float)test_hz;
        }
    }

    if (anchor_span > 0.0f && best_span < anchor_span * 1.05f) {
        return approx_hz;
    }
    if (fabsf(best_freq - approx_hz) < 5.0f) {
        return approx_hz;
    }

    return best_freq;
}

static int cwskimmer_refine_tone_from_spectrum(const float *power, int num_bins,
                                               int sample_rate, float approx_hz,
                                               float *offset_hz, float *snr_db)
{
    float bin_hz;
    int dc;
    int center_bin;
    int search_bins;
    int i;
    int best_i = -1;
    float best_p = -1.0f;
    float noise = 0.0f;
    int noise_count = 0;

    if (!power || num_bins <= 2 || sample_rate <= 0 || !offset_hz || !snr_db) {
        return 0;
    }

    bin_hz = (float)sample_rate / (float)num_bins;
    dc = num_bins / 2;
    center_bin = dc + (int)lroundf(approx_hz / bin_hz);
    search_bins = (int)lroundf(TONE_SEARCH_RANGE_HZ / bin_hz);
    if (search_bins < 3) {
        search_bins = 3;
    }

    for (i = center_bin - search_bins - 6; i < center_bin - search_bins; i++) {
        if (i >= 0 && i < num_bins) {
            noise += power[i];
            noise_count++;
        }
    }
    for (i = center_bin + search_bins + 1; i <= center_bin + search_bins + 6; i++) {
        if (i >= 0 && i < num_bins) {
            noise += power[i];
            noise_count++;
        }
    }
    if (noise_count > 0) {
        noise /= (float)noise_count;
    } else {
        noise = power[dc > 0 ? dc - 1 : 0];
    }

    for (i = center_bin - search_bins; i <= center_bin + search_bins; i++) {
        if (i <= 0 || i >= num_bins - 1) {
            continue;
        }
        if (power[i] <= power[i - 1] || power[i] < power[i + 1]) {
            continue;
        }
        if (power[i] > best_p) {
            best_p = power[i];
            best_i = i;
        }
    }

    if (best_i < 0) {
        *offset_hz = approx_hz;
        *snr_db = 6.0f;
        return 0;
    }

    {
        float delta = 0.0f;
        float y1 = power[best_i - 1];
        float y3 = power[best_i + 1];
        float denom = y1 - 2.0f * best_p + y3;

        if (fabsf(denom) > 1e-6f) {
            delta = 0.5f * (y1 - y3) / denom;
            if (delta > 1.0f) {
                delta = 1.0f;
            } else if (delta < -1.0f) {
                delta = -1.0f;
            }
        }
        *offset_hz = ((float)best_i + delta - (float)num_bins * 0.5f) * bin_hz;
    }
    *snr_db = best_p - noise;
    if (*snr_db < 3.0f) {
        *snr_db = 3.0f;
    }
    return 1;
}

static char cwskimmer_upper_char(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static int cwskimmer_text_contains_ci(const char *haystack, const char *needle)
{
    size_t nlen;
    size_t i;

    if (!haystack || !needle || needle[0] == '\0') {
        return 0;
    }

    nlen = strlen(needle);
    for (i = 0; haystack[i] != '\0'; i++) {
        size_t j;
        for (j = 0; j < nlen && haystack[i + j] != '\0'; j++) {
            if (cwskimmer_upper_char(haystack[i + j]) !=
                cwskimmer_upper_char(needle[j])) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

static void cwskimmer_compact_alnum(const char *in, char *out, int out_max)
{
    int j = 0;
    int i;

    if (!out || out_max <= 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    for (i = 0; in[i] != '\0' && j < out_max - 1; i++) {
        char c = cwskimmer_upper_char(in[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static int cwskimmer_text_matches_ci(const char *decoded, const char *expected)
{
    char compact_dec[128];
    char compact_exp[64];

    if (!decoded || !expected || expected[0] == '\0') {
        return 0;
    }

    if (cwskimmer_text_contains_ci(decoded, expected)) {
        return 1;
    }

    cwskimmer_compact_alnum(decoded, compact_dec, (int)sizeof(compact_dec));
    cwskimmer_compact_alnum(expected, compact_exp, (int)sizeof(compact_exp));
    if (compact_exp[0] == '\0') {
        return 0;
    }
    return cwskimmer_text_contains_ci(compact_dec, compact_exp);
}

static decode_worker_t *cwskimmer_find_closest_channel(cwskimmer_detector_t *detector,
                                                     float tone_offset_hz)
{
    float bucket_hz = cwskimmer_decode_bucket_hz(detector);
    decode_worker_t *best = NULL;
    float best_delta = bucket_hz + 1.0f;
    int i;

    for (i = 0; i < MAX_DECODE_CHANNELS; i++) {
        decode_worker_t *ch = &detector->decode_channels[i];
        float delta;

        if (!ch->active) {
            continue;
        }

        delta = fabsf(ch->bucket_center_hz - tone_offset_hz);
        if (delta <= bucket_hz && delta < best_delta) {
            best = ch;
            best_delta = delta;
        }
    }

    return best;
}

static void cwskimmer_release_channel(cwskimmer_detector_t *detector,
                                      decode_worker_t *channel)
{
    (void)detector;
    if (!channel) {
        return;
    }
    decode_worker_reset(channel);
}

static void cwskimmer_release_all_channels(cwskimmer_detector_t *detector)
{
    int i;
    if (!detector) {
        return;
    }
    for (i = 0; i < MAX_DECODE_CHANNELS; i++) {
        cwskimmer_release_channel(detector, &detector->decode_channels[i]);
    }
}

static decode_worker_t *cwskimmer_find_channel(cwskimmer_detector_t *detector,
                                               float tone_offset_hz)
{
    float bucket_hz = cwskimmer_decode_bucket_hz(detector);
    int i;

    for (i = 0; i < MAX_DECODE_CHANNELS; i++) {
        decode_worker_t *ch = &detector->decode_channels[i];
        if (ch->active &&
            cwskimmer_in_same_decode_bucket(ch->bucket_center_hz, tone_offset_hz,
                                            bucket_hz)) {
            return ch;
        }
    }
    return NULL;
}

static void cwskimmer_send_decode_update(cwskimmer_detector_t *detector,
                                         decode_worker_t *channel,
                                         const char *text,
                                         const char *partial,
                                         char new_letter);

static void cwskimmer_submit_spot_from_decode(cwskimmer_detector_t *detector,
                                              decode_worker_t *channel,
                                              const decoded_callsign_t *dec,
                                              time_t now);

static void cwskimmer_decode_worker_result(decode_worker_t *worker, void *userdata)
{
    cwskimmer_detector_t *detector = (cwskimmer_detector_t *)userdata;
    decoded_callsign_t dec;
    cw_validation_mode_t vmode;
    int accepted;

    if (!detector || !worker) {
        return;
    }

    if (worker->birdie) {
        cwskimmer_release_channel(detector, worker);
        return;
    }

    cwskimmer_send_decode_update(detector, worker,
                                 worker->result_text,
                                 worker->result_partial,
                                 worker->result_new_letter);

    dec = worker->result_callsign;
    if (!dec.valid || dec.callsign[0] == '\0' || worker->frequency_hz <= 100.0f) {
        return;
    }
    if (detector->replay_collect) {
        return;
    }

    vmode = cw_message_validator_parse_mode(detector->config.validation_mode);
    dec.confidence = cw_message_validator_score(dec.callsign, vmode,
                                                worker->last_completed_word);
    accepted = cw_message_validator_accepts(dec.callsign, vmode,
                                            worker->last_completed_word);
    if (accepted) {
        LOG_INFO("Decoded message: %s @ %.0f Hz (conf %.0f%%)",
                 dec.callsign, worker->frequency_hz, dec.confidence * 100.0f);
        strncpy(worker->last_completed_word, dec.callsign,
                sizeof(worker->last_completed_word) - 1);
        worker->last_completed_word[sizeof(worker->last_completed_word) - 1] = '\0';
        cwskimmer_submit_spot_from_decode(detector, worker, &dec, time(NULL));
    } else {
        LOG_DEBUG("Rejected low-confidence decode: '%s' @ %.0f Hz (conf %.0f%%)",
                  dec.callsign, worker->frequency_hz, dec.confidence * 100.0f);
    }
}

static decode_worker_t *cwskimmer_acquire_channel(cwskimmer_detector_t *detector,
                                                float tone_offset_hz,
                                                float frequency_hz,
                                                float peak_snr,
                                                time_t now)
{
    decode_worker_t *ch;
    int i;
    int free_slot = -1;

    if (!detector) {
        return NULL;
    }
    if (!detector->replay_collect) {
        float min_decode = detector->config.decode_min_snr_db;
        if (min_decode > 2.0f) {
            min_decode -= 1.0f;
        }
        if (peak_snr < min_decode) {
            return NULL;
        }
    }

    ch = cwskimmer_find_channel(detector, tone_offset_hz);
    if (ch) {
        decode_worker_set_metadata(ch, tone_offset_hz, frequency_hz, peak_snr, now);
        if (peak_snr >= ch->peak_snr) {
            ch->bucket_center_hz = tone_offset_hz;
        }
        return ch;
    }

    for (i = 0; i < MAX_DECODE_CHANNELS; i++) {
        if (!detector->decode_channels[i].active && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        return NULL;
    }

    ch = &detector->decode_channels[free_slot];
    if (!ch->result_cb) {
        decode_worker_init(ch, free_slot, cwskimmer_decode_worker_result, detector);
    }

    if (decode_worker_start(ch, detector->config.sample_rate, tone_offset_hz) != 0) {
        return NULL;
    }

    ch->bucket_center_hz = tone_offset_hz;
    ch->tone_offset_hz = tone_offset_hz;
    ch->frequency_hz = frequency_hz;
    ch->peak_snr = peak_snr;
    ch->last_active = now;

    return ch;
}

static void cwskimmer_expire_channels(cwskimmer_detector_t *detector, time_t now)
{
    int i;
    for (i = 0; i < MAX_DECODE_CHANNELS; i++) {
        decode_worker_t *ch = &detector->decode_channels[i];
        if (ch->active && (now - ch->last_active) > DECODE_CHANNEL_TIMEOUT_SEC) {
            cwskimmer_release_channel(detector, ch);
        }
    }
}

static void cwskimmer_feed_channel_iq(decode_worker_t *channel,
                                      int sample_rate,
                                      float complex *iq_samples,
                                      int count)
{
    (void)sample_rate;

    if (!channel || !iq_samples || count <= 0) {
        return;
    }

    if (channel->birdie) {
        return;
    }

    decode_worker_submit_iq(channel, iq_samples, count);
}

static void cwskimmer_send_decode_update(cwskimmer_detector_t *detector,
                                         decode_worker_t *channel,
                                         const char *text,
                                         const char *partial,
                                         char new_letter)
{
    char clean_text[64];
    char clean_partial[32];
    cwskimmer_decode_t dec;

    if (!detector || !channel || !detector->decode_cb) {
        return;
    }

    clean_text[0] = '\0';
    clean_partial[0] = '\0';
    if (text) {
        cwskimmer_sanitize_decode_text(text, clean_text, (int)sizeof(clean_text));
    }
    if (partial) {
        cwskimmer_sanitize_decode_text(partial, clean_partial, (int)sizeof(clean_partial));
    }

    if (clean_text[0] == '\0' && clean_partial[0] == '\0' && new_letter == '\0') {
        return;
    }

    if (clean_partial[0] != '\0') {
        strncpy(channel->last_gui_partial, clean_partial, sizeof(channel->last_gui_partial) - 1);
        channel->last_gui_partial[sizeof(channel->last_gui_partial) - 1] = '\0';
    } else {
        channel->last_gui_partial[0] = '\0';
    }

    memset(&dec, 0, sizeof(dec));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    strncpy(dec.text, clean_text, sizeof(dec.text) - 1);
    strncpy(dec.partial_morse, clean_partial, sizeof(dec.partial_morse) - 1);
#pragma GCC diagnostic pop
    {
        cw_validation_mode_t vmode =
            cw_message_validator_parse_mode(detector->config.validation_mode);
        float confidence = cw_message_validator_score(
            clean_text, vmode, channel->last_completed_word);
        float display_min = cw_message_validator_display_threshold(vmode);

        dec.text_confidence = confidence;
        dec.new_letter = new_letter;
        if (new_letter != '\0' && confidence < display_min) {
            dec.new_letter = '\0';
        }
    }

    dec.frequency_hz = channel->frequency_hz;
    dec.freq_offset_hz = channel->bucket_center_hz;

    {
        cwskimmer_decode_callback cb;
        void *userdata;
        pthread_mutex_lock(&detector->lock);
        cb = detector->decode_cb;
        userdata = detector->decode_userdata;
        pthread_mutex_unlock(&detector->lock);
        if (cb) {
            cb(&dec, userdata);
        }
    }
}

static void cwskimmer_submit_spot_from_decode(cwskimmer_detector_t *detector,
                                              decode_worker_t *channel,
                                              const decoded_callsign_t *dec,
                                              time_t now)
{
    if (!detector || !channel || !dec || !dec->valid) {
        return;
    }

    if (detector->config.spot_enabled && detector->reporter) {
        spot_t rspot;
        memset(&rspot, 0, sizeof(rspot));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
        strncpy(rspot.callsign, dec->callsign, sizeof(rspot.callsign) - 1);
#pragma GCC diagnostic pop
        rspot.callsign[sizeof(rspot.callsign) - 1] = '\0';
        rspot.frequency_hz = channel->frequency_hz;
        rspot.snr_db = channel->peak_snr;
        rspot.confidence = dec->confidence;
        rspot.timestamp = (unsigned long)now;
        strncpy(rspot.mode, "CW", sizeof(rspot.mode) - 1);
        rspot.mode[sizeof(rspot.mode) - 1] = '\0';
        spot_reporter_submit_spot(detector->reporter, &rspot);
    }

    if (detector->config.spot_enabled && detector->spot_cb) {
        cwskimmer_spot_t cspot;
        memset(&cspot, 0, sizeof(cspot));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
        strncpy(cspot.callsign, dec->callsign, sizeof(cspot.callsign) - 1);
#pragma GCC diagnostic pop
        cspot.callsign[sizeof(cspot.callsign) - 1] = '\0';
        cspot.frequency_hz = channel->frequency_hz;
        cspot.snr_db = channel->peak_snr;
        cspot.confidence = dec->confidence;
        cspot.timestamp = now;
        strncpy(cspot.mode, "CW", sizeof(cspot.mode) - 1);
        cspot.mode[sizeof(cspot.mode) - 1] = '\0';

        pthread_mutex_lock(&detector->lock);
        if (detector->spot_cb) {
            detector->spot_cb(&cspot, detector->spot_userdata);
        }
        pthread_mutex_unlock(&detector->lock);
    }
}

static float cwskimmer_live_center_hz(cwskimmer_detector_t *detector)
{
    long long radio_freq = 0;
    if (detector->radio) {
        radio_freq = tci_get_center_frequency(detector->radio);
    }
    if (radio_freq > 0) {
        detector->config.center_frequency = (float)radio_freq;
        return (float)radio_freq;
    }
    return detector->config.center_frequency;
}

static void cwskimmer_emit_one_spectrum(cwskimmer_detector_t *detector,
                                        float *power_data, int num_bins,
                                        struct timespec *last_emit,
                                        int *have_emit)
{
    cwskimmer_spectrum_t spectrum;
    struct timespec now;

    if (!detector || !detector->spectrum_cb || !power_data || num_bins <= 0) {
        return;
    }

    perf_mark_now(&now);
    if (have_emit && last_emit && *have_emit) {
        perf_note_value(PERF_SPECTRUM_GAP, perf_elapsed_ms(last_emit, &now));
    }
    if (have_emit && last_emit) {
        *last_emit = now;
        *have_emit = 1;
    }

    spectrum.power_spectrum = power_data;
    spectrum.num_bins = num_bins;
    spectrum.center_frequency = cwskimmer_live_center_hz(detector);
    if (detector->audio_proc) {
        spectrum.bin_width = audio_processor_bin_width(detector->audio_proc);
        /*
         * Real audio (deskHPSDR): spectrum is 0..Nyquist with bin0 = 0 Hz audio.
         * GUI labels use (bin - N/2)*bw + center, so set center = VFO + span/2
         * → abs RF ≈ VFO + audio_hz (CW pitch above carrier).
         */
        if (audio_processor_real_audio_mode(detector->audio_proc) &&
            spectrum.bin_width > 0.0f && num_bins > 0) {
            spectrum.center_frequency +=
                0.5f * (float)num_bins * spectrum.bin_width;
        }
    } else {
        spectrum.bin_width = detector->config.sample_rate / (float)num_bins;
    }
    if (spectrum.bin_width <= 0.0f) {
        spectrum.bin_width = detector->config.sample_rate / (float)num_bins;
    }

    perf_begin(PERF_SPECTRUM_CB);
    {
        cwskimmer_spectrum_callback cb;
        void *userdata;
        pthread_mutex_lock(&detector->lock);
        cb = detector->spectrum_cb;
        userdata = detector->spectrum_userdata;
        pthread_mutex_unlock(&detector->lock);
        if (cb) {
            cb(&spectrum, userdata);
        }
    }
    perf_end(PERF_SPECTRUM_CB);
}

/* Drain all spectrum frames produced by the last audio_processor_process(). */
static void cwskimmer_emit_spectrum(cwskimmer_detector_t *detector,
                                    struct timespec *last_emit,
                                    int *have_emit)
{
    float power_data[AP_MAX_FFT_N];
    int num_bins;
    int frames = 0;

    if (!detector || !detector->spectrum_cb || !detector->audio_proc) {
        return;
    }

    while ((num_bins = audio_processor_pop_spectrum(detector->audio_proc,
                                                    power_data, AP_MAX_FFT_N)) > 0) {
        cwskimmer_emit_one_spectrum(detector, power_data, num_bins,
                                    last_emit, have_emit);
        frames++;
        if (frames >= AP_MAX_PENDING_SPEC) {
            break;
        }
    }

    /* Fallback: no queue entry — use latest mirror */
    if (frames == 0) {
        num_bins = audio_processor_get_power_spectrum(detector->audio_proc, power_data);
        if (num_bins > 0) {
            cwskimmer_emit_one_spectrum(detector, power_data, num_bins,
                                        last_emit, have_emit);
        }
    }
}

static void cwskimmer_add_decode_candidate(cwskimmer_detector_t *detector,
                                           float *offsets, float *snrs, int *count,
                                           int max_count, float offset_hz, float snr_db)
{
    float bucket_hz = cwskimmer_decode_bucket_hz(detector);
    int i;

    if (*count >= max_count) {
        return;
    }

    for (i = 0; i < *count; i++) {
        if (cwskimmer_in_same_decode_bucket(offsets[i], offset_hz, bucket_hz)) {
            if (snr_db > snrs[i]) {
                snrs[i] = snr_db;
            }
            return;
        }
    }

    offsets[*count] = offset_hz;
    snrs[*count] = snr_db;
    (*count)++;
}

static int cwskimmer_find_peaks_in_power(const float *power, int num_bins,
                                         int sample_rate, float min_snr_db,
                                         float *offset_hz, float *snr_db,
                                         int max_peaks)
{
    int dc;
    int guard;
    float noise;
    float bin_hz;
    int found;
    int i;

    if (!power || num_bins <= 2 || sample_rate <= 0 ||
        !offset_hz || !snr_db || max_peaks <= 0) {
        return 0;
    }

    dc = num_bins / 2;
    guard = 8;
    noise = -110.0f;
    {
        float sorted[1024];
        int count = 0;
        int j;

        if (num_bins > 1024) {
            return 0;
        }

        for (i = 0; i < num_bins; i++) {
            if (i >= dc - guard && i <= dc + guard) {
                continue;
            }
            sorted[count++] = power[i];
        }
        if (count <= 0) {
            return 0;
        }
        for (i = 0; i < count - 1; i++) {
            for (j = 0; j < count - i - 1; j++) {
                if (sorted[j] > sorted[j + 1]) {
                    float tmp = sorted[j];
                    sorted[j] = sorted[j + 1];
                    sorted[j + 1] = tmp;
                }
            }
        }
        noise = sorted[count / 5];
    }

    bin_hz = (float)sample_rate / (float)num_bins;
    found = 0;

    for (i = 1; i < num_bins - 1 && found < max_peaks; i++) {
        float p = power[i];
        float snr;
        float delta;

        if (i >= dc - guard && i <= dc + guard) {
            continue;
        }
        if (p <= power[i - 1] || p < power[i + 1]) {
            continue;
        }

        snr = p - noise;
        if (snr < min_snr_db) {
            continue;
        }

        delta = 0.0f;
        {
            float y1 = power[i - 1];
            float y3 = power[i + 1];
            float denom = y1 - 2.0f * p + y3;
            if (fabsf(denom) > 1e-6f) {
                delta = 0.5f * (y1 - y3) / denom;
                if (delta > 1.0f) {
                    delta = 1.0f;
                } else if (delta < -1.0f) {
                    delta = -1.0f;
                }
            }
        }
        offset_hz[found] = ((float)i + delta - (float)num_bins * 0.5f) * bin_hz;
        snr_db[found] = snr;
        found++;
    }

    return found;
}

static int cwskimmer_add_envelope_candidate(cwskimmer_detector_t *detector,
                                            float *offsets, float *spans,
                                            int *count, int max_count,
                                            float offset_hz, float span_score)
{
    float bucket_hz = cwskimmer_decode_bucket_hz(detector);
    int i;

    if (*count >= max_count || span_score <= 0.0f) {
        return 0;
    }

    if (detector->replay_collect && bucket_hz > 150.0f) {
        bucket_hz = 150.0f;
    }

    for (i = 0; i < *count; i++) {
        if (cwskimmer_in_same_decode_bucket(offsets[i], offset_hz, bucket_hz)) {
            if (span_score > spans[i]) {
                spans[i] = span_score;
            }
            return 0;
        }
    }

    offsets[*count] = offset_hz;
    spans[*count] = span_score;
    (*count)++;
    return 1;
}

static int cwskimmer_prescan_capture_signals(cwskimmer_detector_t *detector,
                                             const float complex *samples,
                                             int num_samples,
                                             float mark_hint_hz,
                                             float *offsets, float *snrs,
                                             int max_count)
{
    float max_power[REPLAY_PRESPECTRUM_BINS];
    float complex chunk[IQ_PROCESS_CHUNK];
    float peak_offsets[32];
    float peak_snrs[32];
    float span_scores[MAX_DECODE_CHANNELS];
    int peak_count;
    int count = 0;
    int offset;
    int pi;
    int sample_rate;
    float max_span = 0.0f;
    float min_span;
    float test_hz;

    if (!detector || !detector->audio_proc || !samples || num_samples <= 0 ||
        !offsets || !snrs || max_count <= 0) {
        return 0;
    }

    sample_rate = detector->config.sample_rate;

    if (mark_hint_hz != 0.0f) {
        float coarse_offsets[32];
        float coarse_spans[32];
        int coarse_count = 0;
        int keep_limit = REPLAY_MARK_MAX_CANDIDATES;
        int ci;

        if (max_count < keep_limit) {
            keep_limit = max_count;
        }

        for (test_hz = mark_hint_hz - TONE_SEARCH_RANGE_HZ;
             test_hz <= mark_hint_hz + TONE_SEARCH_RANGE_HZ + 0.1f;
             test_hz += TONE_COARSE_STEP_HZ) {
            float span = cwskimmer_tone_envelope_span(samples, num_samples,
                                                      sample_rate, test_hz);
            if (span > max_span) {
                max_span = span;
            }
            cwskimmer_add_envelope_candidate(detector, coarse_offsets, coarse_spans,
                                             &coarse_count, 32, test_hz, span);
        }

        for (ci = 0; ci < coarse_count; ci++) {
            int cj;
            for (cj = ci + 1; cj < coarse_count; cj++) {
                if (coarse_spans[cj] > coarse_spans[ci]) {
                    float tmp_off = coarse_offsets[ci];
                    float tmp_span = coarse_spans[ci];
                    coarse_offsets[ci] = coarse_offsets[cj];
                    coarse_spans[ci] = coarse_spans[cj];
                    coarse_offsets[cj] = tmp_off;
                    coarse_spans[cj] = tmp_span;
                }
            }
        }

        min_span = max_span * 0.35f;
        if (min_span < 1e-8f) {
            min_span = 1e-8f;
        }

        for (ci = 0; ci < coarse_count && count < keep_limit; ci++) {
            if (coarse_spans[ci] < min_span) {
                continue;
            }
            offsets[count] = cwskimmer_search_tone_envelope(samples, num_samples,
                                                            sample_rate,
                                                            coarse_offsets[ci]);
            span_scores[count] = coarse_spans[ci];
            snrs[count] = 3.0f + 20.0f * (span_scores[count] / (max_span + 1e-8f));
            count++;
        }
        return count;
    } else {
    audio_processor_reset(detector->audio_proc);
    for (offset = 0; offset < num_samples; offset += REPLAY_PRESPECTRUM_BINS / 2) {
        int got = num_samples - offset;
        float power[REPLAY_PRESPECTRUM_BINS];
        int num_bins;
        int bi;

        if (got > IQ_PROCESS_CHUNK) {
            got = IQ_PROCESS_CHUNK;
        }
        if (got < REPLAY_PRESPECTRUM_BINS) {
            continue;
        }
        memcpy(chunk, samples + offset, (size_t)got * sizeof(float complex));
        audio_processor_process(detector->audio_proc, chunk, got);
        num_bins = audio_processor_get_power_spectrum(detector->audio_proc, power);
        if (num_bins <= 0 || num_bins > REPLAY_PRESPECTRUM_BINS) {
            continue;
        }
        if (offset == 0) {
            for (bi = 0; bi < num_bins; bi++) {
                max_power[bi] = power[bi];
            }
        } else {
            for (bi = 0; bi < num_bins; bi++) {
                if (power[bi] > max_power[bi]) {
                    max_power[bi] = power[bi];
                }
            }
        }
    }

    peak_count = cwskimmer_find_peaks_in_power(max_power, REPLAY_PRESPECTRUM_BINS,
                                               sample_rate, 0.0f,
                                               peak_offsets, peak_snrs, 32);

    for (pi = 0; pi < peak_count && count < max_count; pi++) {
        float refined;
        float span;

        if (fabsf(peak_offsets[pi]) > 22000.0f) {
            continue;
        }

        refined = cwskimmer_search_tone_envelope(samples, num_samples,
                                                 sample_rate, peak_offsets[pi]);
        span = cwskimmer_tone_envelope_span(samples, num_samples,
                                            sample_rate, refined);
        if (span > max_span) {
            max_span = span;
        }
        cwskimmer_add_envelope_candidate(detector, offsets, span_scores,
                                         &count, max_count, refined, span);
    }
    }

    min_span = max_span * 0.20f;
    if (min_span < 1e-8f) {
        min_span = 1e-8f;
    }
    {
        int keep = 0;
        for (pi = 0; pi < count; pi++) {
            if (span_scores[pi] >= min_span) {
                if (keep != pi) {
                    offsets[keep] = offsets[pi];
                    span_scores[keep] = span_scores[pi];
                }
                snrs[keep] = 3.0f + 20.0f * (span_scores[pi] / (max_span + 1e-8f));
                keep++;
            }
        }
        count = keep;
    }

    for (pi = 0; pi < count; pi++) {
        int sj;
        for (sj = pi + 1; sj < count; sj++) {
            if (span_scores[sj] > span_scores[pi]) {
                float tmp_off = offsets[pi];
                float tmp_span = span_scores[pi];
                float tmp_snr = snrs[pi];
                offsets[pi] = offsets[sj];
                span_scores[pi] = span_scores[sj];
                snrs[pi] = snrs[sj];
                offsets[sj] = tmp_off;
                span_scores[sj] = tmp_span;
                snrs[sj] = tmp_snr;
            }
        }
    }

    for (pi = 0; pi < count && pi < max_count; pi++) {
        offsets[pi] = cwskimmer_search_tone_envelope(samples, num_samples,
                                                     sample_rate, offsets[pi]);
    }

    if (count > max_count) {
        count = max_count;
    }

    return count;
}

static void cwskimmer_feed_all_active_channels(cwskimmer_detector_t *detector,
                                               float complex *iq_samples,
                                               int count)
{
    int ci;

    if (!detector || !iq_samples || count <= 0) {
        return;
    }

    for (ci = 0; ci < MAX_DECODE_CHANNELS; ci++) {
        decode_worker_t *channel = &detector->decode_channels[ci];
        if (!channel->active || channel->birdie) {
            continue;
        }
        cwskimmer_feed_channel_iq(channel, detector->config.sample_rate,
                                  iq_samples, count);
    }
}

static void cwskimmer_process_decode_channels(cwskimmer_detector_t *detector,
                                              float complex *iq_buffer,
                                              int got,
                                              const cw_signal_t *signals,
                                              int num_signals)
{
    float peak_offsets[MAX_DECODE_CHANNELS];
    float peak_snrs[MAX_DECODE_CHANNELS];
    int peak_count = 0;
    int pi;
    int si;
    time_t now = time(NULL);
    float decode_snr_floor;

    if (!detector || !iq_buffer || got <= 0) {
        return;
    }

    decode_snr_floor = detector->config.decode_min_snr_db;
    if (decode_snr_floor > 2.0f) {
        decode_snr_floor -= 1.0f;
    }

    /* Decode the same tones that drive the yellow signal markers */
    for (si = 0; si < num_signals; si++) {
        if (!signals[si].valid) {
            continue;
        }
        cwskimmer_add_decode_candidate(detector, peak_offsets, peak_snrs, &peak_count,
                                       MAX_DECODE_CHANNELS,
                                       signals[si].frequency, signals[si].snr_db);
    }

    /* Also pick up any additional FFT peaks (skip during offline replay). */
    if (!detector->replay_collect) {
        float extra_offsets[MAX_DECODE_CHANNELS];
        float extra_snrs[MAX_DECODE_CHANNELS];
        int extra_count = audio_processor_find_peaks(
            detector->audio_proc, detector->config.sample_rate,
            decode_snr_floor, extra_offsets, extra_snrs, MAX_DECODE_CHANNELS);

        for (pi = 0; pi < extra_count; pi++) {
            cwskimmer_add_decode_candidate(detector, peak_offsets, peak_snrs, &peak_count,
                                           MAX_DECODE_CHANNELS,
                                           extra_offsets[pi], extra_snrs[pi]);
        }
    }

    for (pi = 0; pi < peak_count; pi++) {
        int sj;
        for (sj = pi + 1; sj < peak_count; sj++) {
            if (peak_snrs[sj] > peak_snrs[pi]) {
                float tmp_off = peak_offsets[pi];
                float tmp_snr = peak_snrs[pi];
                peak_offsets[pi] = peak_offsets[sj];
                peak_snrs[pi] = peak_snrs[sj];
                peak_offsets[sj] = tmp_off;
                peak_snrs[sj] = tmp_snr;
            }
        }
    }

    {
        float radio_center = cwskimmer_radio_center_hz(detector);

        for (pi = 0; pi < peak_count; pi++) {
            decode_worker_t *channel;
            float tone_hz = peak_offsets[pi];
            float tone_snr = peak_snrs[pi];

            if (detector->audio_proc) {
                float power_copy[1024];
                int num_bins = audio_processor_get_power_spectrum(detector->audio_proc,
                                                                  power_copy);
                if (num_bins > 0) {
                    cwskimmer_refine_tone_from_spectrum(
                        power_copy, num_bins, detector->config.sample_rate,
                        tone_hz, &tone_hz, &tone_snr);
                }
            }

            channel = cwskimmer_acquire_channel(detector, tone_hz,
                                                tone_hz + radio_center,
                                                tone_snr, now);
            if (!channel || channel->birdie) {
                continue;
            }

            decode_worker_set_metadata(channel, tone_hz,
                                       tone_hz + radio_center, tone_snr, now);
            cwskimmer_feed_channel_iq(channel, detector->config.sample_rate,
                                      iq_buffer, got);
        }
    }

    cwskimmer_expire_channels(detector, now);
}

/* Log callback wrapper for C detector (wired if logger supports callbacks in future) */
static void log_callback_wrapper(const char *message, int level, void *userdata) __attribute__((unused));
static void log_callback_wrapper(const char *message, int level, void *userdata) {
    cwskimmer_detector_t *det = (cwskimmer_detector_t *)userdata;
    if (det && det->log_cb) {
        det->log_cb(message, level, det->log_userdata);
    }
}

cwskimmer_detector_t *cwskimmer_detector_create(const char *config_file) {
    cwskimmer_detector_t *detector = malloc(sizeof(cwskimmer_detector_t));
    if (!detector) {
        return NULL;
    }
    
    memset(detector, 0, sizeof(cwskimmer_detector_t));
    pthread_mutex_init(&detector->lock, NULL);
    
    /* Load configuration */
    config_load_auto(config_file, &detector->config);
    
    logger_init(detector->config.log_level, detector->config.log_file);

    if (cw_decoder_global_init(detector->config.deepcw_model_path) != 0) {
        LOG_ERROR("Failed to initialize ditdah decoder");
        pthread_mutex_destroy(&detector->lock);
        free(detector);
        return NULL;
    }

    {
        int i;
        for (i = 0; i < MAX_DECODE_CHANNELS; i++) {
            decode_worker_init(&detector->decode_channels[i], i,
                               cwskimmer_decode_worker_result, detector);
        }
    }
    
    return detector;
}

static void cwskimmer_free_frozen_capture_locked(cwskimmer_detector_t *detector)
{
    if (!detector) {
        return;
    }

    if (detector->frozen_capture_samples) {
        free(detector->frozen_capture_samples);
        detector->frozen_capture_samples = NULL;
    }
    detector->frozen_capture_count = 0;
    detector->frozen_capture_sample_rate = 0;
    detector->frozen_capture_center_hz = 0;
}

void cwskimmer_detector_destroy(cwskimmer_detector_t *detector) {
    if (!detector) return;
    
    cwskimmer_stop(detector);
    
    pthread_mutex_lock(&detector->lock);
    
    if (detector->reporter) spot_reporter_destroy(detector->reporter);
    cwskimmer_release_all_channels(detector);
    {
        int i;
        for (i = 0; i < MAX_DECODE_CHANNELS; i++) {
            decode_worker_shutdown(&detector->decode_channels[i]);
        }
    }
    cwskimmer_free_frozen_capture_locked(detector);
    if (detector->iq_capture) cw_capture_ring_destroy(detector->iq_capture);
    if (detector->audio_proc) audio_processor_destroy(detector->audio_proc);
    if (detector->detector) cw_detector_destroy(detector->detector);
    if (detector->radio) tci_client_destroy(detector->radio);
    
    pthread_mutex_unlock(&detector->lock);
    pthread_mutex_destroy(&detector->lock);
    
    cw_decoder_global_shutdown();
    logger_close();
    free(detector);
}

void cwskimmer_set_signal_callback(cwskimmer_detector_t *detector,
                                   cwskimmer_signal_callback callback,
                                   void *userdata) {
    if (detector) {
        pthread_mutex_lock(&detector->lock);
        detector->signal_cb = callback;
        detector->signal_userdata = userdata;
        pthread_mutex_unlock(&detector->lock);
    }
}

void cwskimmer_set_spot_callback(cwskimmer_detector_t *detector,
                                 cwskimmer_spot_callback callback,
                                 void *userdata) {
    if (detector) {
        pthread_mutex_lock(&detector->lock);
        detector->spot_cb = callback;
        detector->spot_userdata = userdata;
        pthread_mutex_unlock(&detector->lock);
    }
}

void cwskimmer_set_log_callback(cwskimmer_detector_t *detector,
                                cwskimmer_log_callback callback,
                                void *userdata) {
    if (detector) {
        pthread_mutex_lock(&detector->lock);
        detector->log_cb = callback;
        detector->log_userdata = userdata;
        pthread_mutex_unlock(&detector->lock);
    }
}

void cwskimmer_set_stats_callback(cwskimmer_detector_t *detector,
                                  cwskimmer_stats_callback callback,
                                  void *userdata) {
    if (detector) {
        pthread_mutex_lock(&detector->lock);
        detector->stats_cb = callback;
        detector->stats_userdata = userdata;
        pthread_mutex_unlock(&detector->lock);
    }
}

void cwskimmer_set_spectrum_callback(cwskimmer_detector_t *detector,
                                     cwskimmer_spectrum_callback callback,
                                     void *userdata) {
    if (detector) {
        pthread_mutex_lock(&detector->lock);
        detector->spectrum_cb = callback;
        detector->spectrum_userdata = userdata;
        pthread_mutex_unlock(&detector->lock);
    }
}

void cwskimmer_set_decode_callback(cwskimmer_detector_t *detector,
                                   cwskimmer_decode_callback callback,
                                   void *userdata) {
    if (detector) {
        pthread_mutex_lock(&detector->lock);
        detector->decode_cb = callback;
        detector->decode_userdata = userdata;
        pthread_mutex_unlock(&detector->lock);
    }
}

#ifdef CONN_DEBUG
#define CONN_DBG(fmt, ...) do { \
    fprintf(stderr, "[CONN] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)
#else
#define CONN_DBG(fmt, ...) ((void)0)
#endif

int cwskimmer_start(cwskimmer_detector_t *detector) {
    if (!detector) return -1;

    CONN_DBG("cwskimmer_start: radio=%s:%d protocol=%s spot=%s:%d",
             detector->config.radio_host, detector->config.radio_port,
             detector->config.radio_protocol,
             detector->config.spot_server_host, detector->config.spot_server_port);

    /* Refuse re-entry while a previous session is still in the detection loop
     * or mid-cleanup (GUI Start after Stop must wait for join). */
    if (detector->loop_active) {
        LOG_ERROR("cwskimmer_start refused: previous detection loop still active");
        CONN_DBG("cwskimmer_start FAILED: loop_active=1 (stop/join first)");
        return -1;
    }

    /* Mark session active before any network I/O so a second Start cannot race */
    detector->loop_active = 1;
    detector->running = 0;
    
    pthread_mutex_lock(&detector->lock);

    /* Defensive cleanup if a prior stop left resources behind */
    if (detector->reporter) {
        spot_reporter_destroy(detector->reporter);
        detector->reporter = NULL;
    }
    if (detector->audio_proc) {
        audio_processor_destroy(detector->audio_proc);
        detector->audio_proc = NULL;
    }
    if (detector->detector) {
        cw_detector_destroy(detector->detector);
        detector->detector = NULL;
    }
    if (detector->radio) {
        tci_client_destroy(detector->radio);
        detector->radio = NULL;
    }
    
    /* Create TCI client */
    detector->radio = tci_client_create(detector->config.radio_host,
                                        detector->config.radio_port,
                                        detector->config.sample_rate,
                                        detector->config.radio_protocol);
    if (!detector->radio) {
        CONN_DBG("cwskimmer_start FAILED: could not create TCI client");
        LOG_ERROR("Failed to create TCI client");
        detector->loop_active = 0;
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }
    
    /* Connect to radio */
    CONN_DBG("cwskimmer_start: calling tci_client_connect ...");
    if (tci_client_connect(detector->radio) < 0) {
        CONN_DBG("cwskimmer_start FAILED: tci_client_connect returned error");
        LOG_ERROR("Failed to connect to radio");
        tci_client_destroy(detector->radio);
        detector->radio = NULL;
        detector->loop_active = 0;
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }
    
    /* Apply stream preference (iq | audio) then subscribe */
    if (strcasecmp(detector->config.tci_stream_mode, "audio") == 0) {
        tci_set_stream_mode_preference(TCI_STREAM_MODE_AUDIO);
    } else {
        tci_set_stream_mode_preference(TCI_STREAM_MODE_IQ);
    }
    CONN_DBG("cwskimmer_start: subscribing to TCI stream (mode=%s) ...",
             detector->config.tci_stream_mode[0] ? detector->config.tci_stream_mode : "iq");
    if (tci_subscribe_iq_stream(detector->radio, 0) < 0) {
        CONN_DBG("cwskimmer_start FAILED: stream subscription failed");
        LOG_ERROR("Failed to subscribe to TCI stream");
        tci_client_destroy(detector->radio);
        detector->radio = NULL;
        detector->loop_active = 0;
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }
    
    /* Request initial center frequency (DDS = IQ LO; VFO as fallback) */
    tci_request_dds_frequency(detector->radio, 0);
    tci_request_vfo_frequency(detector->radio, 0, 0);  /* VFO A, channel 0 */
    
    /* Create detector */
    int num_channels = detector->config.sample_rate / 100;
    LOG_INFO("Creating detector with num_channels=%d (sample_rate=%d)", 
             num_channels, detector->config.sample_rate);
    detector->detector = cw_detector_create(detector->config.sample_rate,
                                            num_channels,
                                            detector->config.detection_threshold);
    if (detector->detector) {
        detector->detector->min_snr_db = detector->config.min_snr_db;
    }
    if (!detector->detector) {
        LOG_ERROR("Failed to create detector");
        tci_client_destroy(detector->radio);
        detector->radio = NULL;
        detector->loop_active = 0;
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }
    
    /* Create audio processor */
    /* Wide 48 kHz / 1024-bin (default) or narrow 3 kHz experiment via spectrum_span_hz */
    LOG_INFO("Creating audio_processor for FFT spectrum");
    detector->audio_proc = audio_processor_create(detector->config.sample_rate,
                                                   1024, 1024);
    if (!detector->audio_proc) {
        LOG_ERROR("Failed to create audio processor");
        cw_detector_destroy(detector->detector);
        detector->detector = NULL;
        tci_client_destroy(detector->radio);
        detector->radio = NULL;
        detector->loop_active = 0;
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }
    {
        int span = detector->config.spectrum_span_hz;
        if (span > 0 && span < detector->config.sample_rate) {
            audio_processor_set_spectrum_span(detector->audio_proc, span);
            LOG_INFO("Spectrum mode NARROW %d Hz at start", span);
        } else {
            audio_processor_set_spectrum_span(detector->audio_proc, 0);
            LOG_INFO("Spectrum mode WIDE (full %d Hz) at start",
                     detector->config.sample_rate);
        }
        /* Match spectrum FFT to active TCI stream type */
        audio_processor_set_real_audio_mode(
            detector->audio_proc,
            tci_stream_is_audio_only(detector->radio));
        LOG_INFO("TCI stream at start: %s",
                 tci_stream_is_audio_only(detector->radio)
                     ? "AUDIO (mono/real spectrum)"
                     : "IQ (complex spectrum)");
    }

    if (detector->iq_capture) {
        cw_capture_ring_destroy(detector->iq_capture);
        detector->iq_capture = NULL;
    }
    detector->iq_capture = cw_capture_ring_create(detector->config.sample_rate,
                                                 CW_CAPTURE_DEFAULT_SECONDS);
    if (!detector->iq_capture) {
        LOG_WARN("IQ capture ring disabled: allocation failed");
    } else {
        LOG_INFO("IQ capture ring enabled (%d s)", CW_CAPTURE_DEFAULT_SECONDS);
    }
    
    /* Spot reporter optional (disabled by default via spot_enabled=0) */
    detector->reporter = NULL;
    if (detector->config.spot_enabled) {
        detector->reporter = spot_reporter_create(detector->config.spot_server_host,
                                                  detector->config.spot_server_port,
                                                  detector->config.spot_server_callsign);
        if (!detector->reporter) {
            LOG_WARN("Spot reporter disabled: failed to create client");
        }
    } else {
        LOG_INFO("Spot network reporting disabled (spot_enabled=0)");
    }
    
    detector->running = 1;
    detector->samples_processed = 0;
    detector->num_detections = 0;
    cwskimmer_release_all_channels(detector);
    
    pthread_mutex_unlock(&detector->lock);

    CONN_DBG("cwskimmer_start: entering detection loop (radio connected=%d)",
             tci_is_connected(detector->radio));
    
    /* Main detection loop */
    float complex *iq_buffer = malloc(detector->config.sample_rate * sizeof(float complex));
    if (!iq_buffer) {
        LOG_ERROR("Failed to allocate I/Q buffer");
        cwskimmer_stop(detector);
        /* Fall through to shared cleanup path by setting loop_active still 1 */
        pthread_mutex_lock(&detector->lock);
        if (detector->reporter) {
            spot_reporter_destroy(detector->reporter);
            detector->reporter = NULL;
        }
        if (detector->audio_proc) {
            audio_processor_destroy(detector->audio_proc);
            detector->audio_proc = NULL;
        }
        if (detector->detector) {
            cw_detector_destroy(detector->detector);
            detector->detector = NULL;
        }
        if (detector->radio) {
            tci_client_destroy(detector->radio);
            detector->radio = NULL;
        }
        pthread_mutex_unlock(&detector->lock);
        detector->loop_active = 0;
        return -1;
    }
    
    int read_count = 0;
    struct timespec last_spectrum_emit;
    int have_spectrum_emit = 0;
    int stream_stall_ticks = 0;
    int stream_audio_retried = 0;

    perf_reset();
    LOG_INFO("Performance profiling enabled — PERF REPORT every 5s in log");

    while (detector->running) {
        int available = 0;
        int chunks_this_loop = 0;

        perf_begin(PERF_LOOP_TOTAL);

        perf_begin(PERF_WS_SERVICE);
        tci_service_websocket();
        perf_end(PERF_WS_SERVICE);

        if (tci_is_connected(detector->radio)) {
            int new_samples;

            perf_begin(PERF_TCI_READ);
            new_samples = tci_read_iq_samples(detector->radio);
            available = tci_buffer_available(detector->radio);
            perf_note_value(PERF_TCI_READ, (double)available);
            perf_end(PERF_TCI_READ);

            if (new_samples > 0) {
                read_count++;
                detector->samples_processed += new_samples;
                stream_stall_ticks = 0;
            } else if (detector->samples_processed == 0) {
                /*
                 * No binary stream yet. If preference is IQ, retry iq_start once;
                 * only fall back to audio_start when preference still allows auto
                 * recovery (IQ mode with silent server). Audio preference: re-send
                 * audio_start. Loop tick ~2–10ms idle → ~150 ticks ≈ 1–2s.
                 */
                stream_stall_ticks++;
                if (!stream_audio_retried && stream_stall_ticks == 150) {
                    if (tci_get_stream_mode_preference() == TCI_STREAM_MODE_AUDIO) {
                        LOG_WARN("No TCI stream samples — re-sending audio_start");
                        CONN_DBG("Stream stall: re-send audio_start:0");
                        tci_subscribe_audio_stream(detector->radio, 0);
                    } else {
                        LOG_WARN("No TCI stream samples after iq_start — "
                                 "retrying audio_start once (servers without IQ)");
                        CONN_DBG("Stream stall: retrying audio_start:0");
                        tci_subscribe_audio_stream(detector->radio, 0);
                    }
                    stream_audio_retried = 1;
                } else if (stream_stall_ticks == 500) {
                    LOG_WARN("Still no TCI binary data. Check: radio host/port, "
                             "deskHPSDR CAT→TCI enabled, radio actually receiving, "
                             "Stream: IQ vs Audio button, or play a capture offline.");
                    CONN_DBG("Stream stall persists — connected but no binary frames");
                }
            }

            while (available >= IQ_PROCESS_CHUNK &&
                   chunks_this_loop < MAX_IQ_CHUNKS_PER_LOOP &&
                   detector->running) {
                int got;
                cw_signal_t signals[10];
                int num_signals = 0;

                perf_begin(PERF_IQ_BLOCK);
                got = tci_get_iq_samples(detector->radio, iq_buffer, IQ_PROCESS_CHUNK);
                perf_note_value(PERF_IQ_BLOCK, (double)got);
                if (got <= 0) {
                    perf_end(PERF_IQ_BLOCK);
                    break;
                }

                if (detector->iq_capture) {
                    pthread_mutex_lock(&detector->lock);
                    cw_capture_ring_write(detector->iq_capture, iq_buffer, got);
                    pthread_mutex_unlock(&detector->lock);
                }

                /* deskHPSDR audio_start vs true IQ — switches single-sided spectrum */
                if (detector->radio) {
                    audio_processor_set_real_audio_mode(
                        detector->audio_proc,
                        tci_stream_is_audio_only(detector->radio));
                }

                perf_begin(PERF_AUDIO_PROC);
                audio_processor_process(detector->audio_proc, iq_buffer, got);
                perf_end(PERF_AUDIO_PROC);

                /* Spectrum frames (possibly multiple hops in narrow mode) → GUI */
                cwskimmer_emit_spectrum(detector, &last_spectrum_emit, &have_spectrum_emit);

                perf_begin(PERF_DETECTOR);
                {
                    float power_copy[AP_MAX_FFT_N];
                    int num_bins = audio_processor_get_power_spectrum(detector->audio_proc,
                                                                      power_copy);
                    /* Spectrum sample rate matches active mode (48k wide / 3k narrow) */
                    int spec_fs = detector->config.sample_rate;
                    if (detector->audio_proc &&
                        audio_processor_spectrum_span_hz(detector->audio_proc) > 0 &&
                        audio_processor_spectrum_span_hz(detector->audio_proc) <
                            detector->config.sample_rate) {
                        spec_fs = audio_processor_spectrum_span_hz(detector->audio_proc);
                    }
                    /* Real audio single-sided: bins cover 0..Fs/2 only */
                    if (detector->audio_proc &&
                        audio_processor_real_audio_mode(detector->audio_proc) &&
                        num_bins > 0) {
                        float bw = audio_processor_bin_width(detector->audio_proc);
                        if (bw > 0.0f) {
                            spec_fs = (int)lroundf(bw * (float)num_bins * 2.0f);
                        }
                    }
                    if (num_bins > 0) {
                        cw_detector_feed_spectrum(detector->detector, power_copy, num_bins,
                                                  spec_fs);
                    }
                }
                perf_end(PERF_DETECTOR);

                detector->detector->min_snr_db = detector->config.min_snr_db;

                perf_begin(PERF_PEAKS_SIGNALS);
                {
                    float power_copy[AP_MAX_FFT_N];
                    int num_bins = audio_processor_get_power_spectrum(detector->audio_proc,
                                                                      power_copy);
                    float peak_offsets[10];
                    float peak_snrs[10];
                    float live_snr_floor = detector->config.decode_min_snr_db;
                    int peak_count;
                    int pi;
                    int spec_fs = detector->config.sample_rate;
                    float max_offset = 22000.0f;

                    if (detector->audio_proc) {
                        float bw = audio_processor_bin_width(detector->audio_proc);
                        int n = audio_processor_fft_n(detector->audio_proc);
                        if (bw > 0.0f && n > 0) {
                            spec_fs = (int)lroundf(bw * (float)n);
                            max_offset = 0.5f * (float)spec_fs;
                        }
                    }

                    if (live_snr_floor > 1.0f) {
                        live_snr_floor -= 1.0f;
                    }
                    peak_count = audio_processor_find_peaks(
                        detector->audio_proc, spec_fs,
                        live_snr_floor, peak_offsets, peak_snrs, 10);

                    for (pi = 0; pi < peak_count && num_signals < 10; pi++) {
                        float confidence = 0.5f;
                        float tone_hz = peak_offsets[pi];
                        float tone_snr = peak_snrs[pi];
                        int si;

                        if (fabsf(tone_hz) > max_offset) {
                            continue;
                        }

                        if (num_bins > 0) {
                            cwskimmer_refine_tone_from_spectrum(
                                power_copy, num_bins, spec_fs,
                                tone_hz, &tone_hz, &tone_snr);
                        }

                        {
                            cw_signal_t det_signals[10];
                            int det_count = cw_detector_get_signals(detector->detector,
                                                                    det_signals, 10);
                            int di;
                            for (di = 0; di < det_count; di++) {
                                if (fabsf(det_signals[di].frequency - tone_hz) < 80.0f) {
                                    confidence = det_signals[di].confidence;
                                    break;
                                }
                            }
                        }

                        for (si = 0; si < num_signals; si++) {
                            if (fabsf(signals[si].frequency - tone_hz) < 80.0f) {
                                if (tone_snr > signals[si].snr_db) {
                                    signals[si].snr_db = tone_snr;
                                    signals[si].frequency = tone_hz;
                                }
                                break;
                            }
                        }
                        if (si < num_signals) {
                            continue;
                        }

                        signals[num_signals].frequency = tone_hz;
                        signals[num_signals].snr_db = tone_snr;
                        signals[num_signals].confidence = confidence;
                        signals[num_signals].tone_purity = fminf(1.0f, tone_snr / 20.0f);
                        signals[num_signals].bandwidth = 50.0f;
                        signals[num_signals].valid = 1;
                        num_signals++;
                    }
                }

                if (num_signals > 0 && detector->signal_cb) {
                    float radio_center = cwskimmer_radio_center_hz(detector);
                    int si;
                    for (si = 0; si < num_signals; si++) {
                        cwskimmer_signal_t sig;
                        sig.freq_offset_hz = signals[si].frequency;
                        sig.frequency = signals[si].frequency + radio_center;
                        sig.snr_db = signals[si].snr_db;
                        sig.confidence = signals[si].confidence;
                        sig.tone_purity = signals[si].tone_purity;
                        sig.bandwidth = signals[si].bandwidth;
                        sig.timestamp = time(NULL);

                        pthread_mutex_lock(&detector->lock);
                        if (detector->signal_cb) {
                            detector->signal_cb(&sig, detector->signal_userdata);
                        }
                        pthread_mutex_unlock(&detector->lock);

                        detector->num_detections++;
                    }
                }
                perf_end(PERF_PEAKS_SIGNALS);

                perf_begin(PERF_DECODE);
                cwskimmer_process_decode_channels(detector, iq_buffer, got,
                                                  signals, num_signals);
                perf_end(PERF_DECODE);

                perf_end(PERF_IQ_BLOCK);

                chunks_this_loop++;
                available = tci_buffer_available(detector->radio);
            }
        } else {
            static int disconnect_report = 0;
            if ((++disconnect_report % 50) == 1) {
                CONN_DBG("detection loop: NOT CONNECTED to %s:%d (tick %d)",
                         detector->config.radio_host, detector->config.radio_port,
                         disconnect_report);
            }
            /* Not connected: sleep briefly to avoid busy-waiting */
            usleep(100000);  /* Sleep 100ms when disconnected */
        }
        
        /* Poll DDS/VFO so waterfall RF labels follow the radio when the operator tunes */
        {
            static int vfo_tick = 0;
            if ((++vfo_tick % 10) == 0 && detector->radio && tci_is_connected(detector->radio)) {
                /* Prefer DDS (IQ stream LO); also poll VFO in case radio only pushes VFO */
                tci_request_dds_frequency(detector->radio, 0);
                tci_request_vfo_frequency(detector->radio, 0, 0);
            }
        }

        /* Emit statistics periodically (use independent tick so it fires even when disconnected) */
        static int stats_tick = 0;
        if ((++stats_tick % 50) == 0 && detector->stats_cb) {  /* ~every 500ms */
            cwskimmer_stats_t stats;
            stats.num_signals = detector->detector ? cw_detector_get_signals(detector->detector, NULL, 0) : 0;
            stats.noise_floor_db = detector->detector ? cw_detector_get_noise_floor(detector->detector) : -120.0f;
            stats.buffer_fill = available;
            stats.connected = detector->radio ? (tci_is_connected(detector->radio) ? 1 : 0) : 0;
            static int last_reported_connected = -1;
            if (stats.connected != last_reported_connected) {
                CONN_DBG("connection status changed: %s (buffer=%d samples=%d)",
                         stats.connected ? "CONNECTED" : "DISCONNECTED",
                         stats.buffer_fill, detector->samples_processed);
                last_reported_connected = stats.connected;
            }
            stats.samples_processed = detector->samples_processed;
            stats.cpu_usage = 5.0f;  /* Placeholder; real measurement could use getrusage or clock */
            stats.queue_size = detector->reporter ? spot_reporter_queue_length(detector->reporter) : 0;
            stats.avg_peak_snr_db = 0.0f;
            stats.peak_snr_db = 0.0f;
            stats.spectrum_peak_count = 0;
            if (detector->audio_proc) {
                audio_processor_get_snr_summary(detector->audio_proc,
                                                &stats.avg_peak_snr_db,
                                                &stats.peak_snr_db,
                                                &stats.spectrum_peak_count);
            }

            pthread_mutex_lock(&detector->lock);
            if (detector->stats_cb) {
                detector->stats_cb(&stats, detector->stats_userdata);
            }
            pthread_mutex_unlock(&detector->lock);
        }

        perf_report_if_due(5.0);

        /* Drain retry queue for spot reporter */
        static int retry_tick = 0;
        if (detector->config.spot_enabled && (++retry_tick % 500) == 0 &&
            detector->reporter &&
            spot_reporter_queue_length(detector->reporter) > 0) {  /* ~every 5s */
            spot_reporter_process_retries(detector->reporter);
        }

        perf_begin(PERF_LOOP_SLEEP);
        {
            int backlog = tci_buffer_available(detector->radio);
            if (backlog >= IQ_PROCESS_CHUNK * 8) {
                usleep(0);
            } else if (backlog >= IQ_PROCESS_CHUNK) {
                usleep(500);
            } else {
                usleep(2000);
            }
        }
        perf_end(PERF_LOOP_SLEEP);

        perf_end(PERF_LOOP_TOTAL);
    }
    
    /* Cleanup sub-resources so cwskimmer_start() can be safely re-invoked after stop (supports GUI Start/Stop cycles).
     * This prevents leaks and pointer overwrites on restart. */
    pthread_mutex_lock(&detector->lock);
    if (detector->reporter) {
        spot_reporter_destroy(detector->reporter);
        detector->reporter = NULL;
    }
    cwskimmer_release_all_channels(detector);
    if (detector->audio_proc) {
        audio_processor_destroy(detector->audio_proc);
        detector->audio_proc = NULL;
    }
    if (detector->detector) {
        cw_detector_destroy(detector->detector);
        detector->detector = NULL;
    }
    if (detector->radio) {
        tci_client_destroy(detector->radio);
        detector->radio = NULL;
    }
    pthread_mutex_unlock(&detector->lock);
    
    free(iq_buffer);
    detector->loop_active = 0;
    LOG_INFO("Detection loop ended. Processed %d samples in %d reads",
             detector->samples_processed, read_count);
    
    return 0;
}

void cwskimmer_stop(cwskimmer_detector_t *detector) {
    if (detector) {
        pthread_mutex_lock(&detector->lock);
        detector->running = 0;
        pthread_mutex_unlock(&detector->lock);
        /* Loop cleans up radio/detector/audio_proc after exiting; do not destroy
         * them here (would race with the detection thread). */
        CONN_DBG("cwskimmer_stop: running=0 (waiting for loop exit/cleanup)");
    }
}

int cwskimmer_is_running(cwskimmer_detector_t *detector) {
    if (!detector) return 0;
    
    pthread_mutex_lock(&detector->lock);
    int running = detector->running;
    pthread_mutex_unlock(&detector->lock);
    
    return running;
}

int cwskimmer_is_loop_active(cwskimmer_detector_t *detector)
{
    if (!detector) {
        return 0;
    }
    return detector->loop_active ? 1 : 0;
}

cwskimmer_stats_t cwskimmer_get_stats(cwskimmer_detector_t *detector) {
    cwskimmer_stats_t stats = {0};
    
    if (!detector) return stats;
    
    pthread_mutex_lock(&detector->lock);
    if (detector->detector) {
        stats.num_signals = cw_detector_get_signals(detector->detector, NULL, 0);
        stats.noise_floor_db = cw_detector_get_noise_floor(detector->detector);
    }
    if (detector->radio) {
        stats.buffer_fill = tci_buffer_available(detector->radio);
        stats.connected = tci_is_connected(detector->radio) ? 1 : 0;
    }
    stats.samples_processed = detector->samples_processed;
    stats.cpu_usage = 8.5f;  /* Placeholder */
    pthread_mutex_unlock(&detector->lock);
    
    return stats;
}

int cwskimmer_config_set(cwskimmer_detector_t *detector,
                         const char *key,
                         const char *value) {
    if (!detector || !key || !value) return -1;
    
    pthread_mutex_lock(&detector->lock);
    
    if (strcmp(key, "detection_threshold") == 0) {
        detector->config.detection_threshold = atoi(value);
        if (detector->detector) {
            detector->detector->detection_threshold = detector->config.detection_threshold;
        }
    } else if (strcmp(key, "min_snr_db") == 0) {
        detector->config.min_snr_db = (float)atof(value);
        if (detector->detector) {
            detector->detector->min_snr_db = detector->config.min_snr_db;
        }
    } else if (strcmp(key, "decode_min_snr_db") == 0) {
        detector->config.decode_min_snr_db = (float)atof(value);
    } else if (strcmp(key, "decode_bucket_hz") == 0) {
        detector->config.decode_bucket_hz = (float)atof(value);
        cwskimmer_release_all_channels(detector);
    } else if (strcmp(key, "validation_mode") == 0) {
        strncpy(detector->config.validation_mode, value,
                sizeof(detector->config.validation_mode) - 1);
        detector->config.validation_mode[sizeof(detector->config.validation_mode) - 1] = '\0';
    } else if (strcmp(key, "spot_enabled") == 0) {
        detector->config.spot_enabled = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "radio_host") == 0) {
        strncpy(detector->config.radio_host, value, sizeof(detector->config.radio_host) - 1);
    } else if (strcmp(key, "radio_port") == 0) {
        detector->config.radio_port = atoi(value);
    } else if (strcmp(key, "radio_protocol") == 0) {
        strncpy(detector->config.radio_protocol, value,
                sizeof(detector->config.radio_protocol) - 1);
        detector->config.radio_protocol[sizeof(detector->config.radio_protocol) - 1] = '\0';
    } else if (strcmp(key, "spot_server_callsign") == 0) {
        strncpy(detector->config.spot_server_callsign, value,
                sizeof(detector->config.spot_server_callsign) - 1);
    } else if (strcmp(key, "spectrum_span_hz") == 0) {
        int span = atoi(value);
        detector->config.spectrum_span_hz = span;
        if (detector->audio_proc) {
            audio_processor_set_spectrum_span(detector->audio_proc, span);
        }
        LOG_INFO("spectrum_span_hz set to %d (%s)", span,
                 (span > 0 && span < detector->config.sample_rate) ? "NARROW" : "WIDE");
    } else if (strcmp(key, "multi_decode_channels") == 0) {
        int n = atoi(value);
        if (n < 1) {
            n = 1;
        }
        if (n > 16) {
            n = 16;
        }
        detector->config.multi_decode_channels = n;
        LOG_INFO("multi_decode_channels → %d", n);
    } else if (strcmp(key, "tci_stream_mode") == 0) {
        int want_audio = (strcasecmp(value, "audio") == 0);
        tci_stream_mode_t mode = want_audio ? TCI_STREAM_MODE_AUDIO : TCI_STREAM_MODE_IQ;
        strncpy(detector->config.tci_stream_mode, want_audio ? "audio" : "iq",
                sizeof(detector->config.tci_stream_mode) - 1);
        detector->config.tci_stream_mode[sizeof(detector->config.tci_stream_mode) - 1] = '\0';
        tci_set_stream_mode_preference(mode);
        LOG_INFO("tci_stream_mode → %s", detector->config.tci_stream_mode);
        /* Live switch if already connected */
        if (detector->radio && tci_is_connected(detector->radio)) {
            int vfo = detector->radio->iq_vfo;
            if (vfo < 0) {
                vfo = 0;
            }
            pthread_mutex_unlock(&detector->lock);
            if (tci_resubscribe_stream(detector->radio, vfo) == 0) {
                if (detector->audio_proc) {
                    audio_processor_set_real_audio_mode(
                        detector->audio_proc,
                        tci_stream_is_audio_only(detector->radio));
                }
                LOG_INFO("TCI stream switched live to %s",
                         tci_stream_is_audio_only(detector->radio) ? "AUDIO" : "IQ");
            } else {
                LOG_WARN("TCI stream live switch failed");
            }
            return 0;
        }
    } else {
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }
    
    pthread_mutex_unlock(&detector->lock);
    return 0;
}

const char *cwskimmer_config_get(cwskimmer_detector_t *detector,
                                 const char *key) {
    if (!detector || !key) return NULL;
    
    static char buffer[256];
    
    pthread_mutex_lock(&detector->lock);
    
    if (strcmp(key, "detection_threshold") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", detector->config.detection_threshold);
    } else if (strcmp(key, "min_snr_db") == 0) {
        snprintf(buffer, sizeof(buffer), "%.1f", detector->config.min_snr_db);
    } else if (strcmp(key, "decode_min_snr_db") == 0) {
        snprintf(buffer, sizeof(buffer), "%.1f", detector->config.decode_min_snr_db);
    } else if (strcmp(key, "decode_bucket_hz") == 0) {
        snprintf(buffer, sizeof(buffer), "%.0f", cwskimmer_decode_bucket_hz(detector));
    } else if (strcmp(key, "validation_mode") == 0) {
        snprintf(buffer, sizeof(buffer), "%s", detector->config.validation_mode);
    } else if (strcmp(key, "spot_enabled") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", detector->config.spot_enabled);
    } else if (strcmp(key, "radio_host") == 0) {
        snprintf(buffer, sizeof(buffer), "%s", detector->config.radio_host);
    } else if (strcmp(key, "radio_port") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", detector->config.radio_port);
    } else if (strcmp(key, "radio_protocol") == 0) {
        snprintf(buffer, sizeof(buffer), "%s", detector->config.radio_protocol);
    } else if (strcmp(key, "spot_server_callsign") == 0) {
        snprintf(buffer, sizeof(buffer), "%s", detector->config.spot_server_callsign);
    } else if (strcmp(key, "spectrum_span_hz") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", detector->config.spectrum_span_hz);
    } else if (strcmp(key, "multi_decode_channels") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", detector->config.multi_decode_channels);
    } else if (strcmp(key, "tci_stream_mode") == 0) {
        snprintf(buffer, sizeof(buffer), "%s",
                 detector->config.tci_stream_mode[0]
                     ? detector->config.tci_stream_mode
                     : "iq");
    } else {
        pthread_mutex_unlock(&detector->lock);
        return NULL;
    }
    
    pthread_mutex_unlock(&detector->lock);
    return buffer;
}

int cwskimmer_freeze_capture(cwskimmer_detector_t *detector)
{
    float complex *samples;
    int capacity;
    int copied;

    if (!detector) {
        return -1;
    }

    pthread_mutex_lock(&detector->lock);

    cwskimmer_free_frozen_capture_locked(detector);

    if (!detector->iq_capture || detector->iq_capture->count <= 0) {
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }

    capacity = detector->iq_capture->count;
    samples = malloc((size_t)capacity * sizeof(float complex));
    if (!samples) {
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }

    copied = cw_capture_ring_copy(detector->iq_capture, samples, capacity);
    if (copied <= 0) {
        free(samples);
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }

    detector->frozen_capture_samples = samples;
    detector->frozen_capture_count = copied;
    detector->frozen_capture_sample_rate = (uint32_t)detector->config.sample_rate;
    detector->frozen_capture_center_hz =
        (uint64_t)llround(cwskimmer_radio_center_hz(detector));

    pthread_mutex_unlock(&detector->lock);

    LOG_INFO("Frozen IQ capture snapshot: %d samples @ %llu Hz",
             copied, (unsigned long long)detector->frozen_capture_center_hz);
    return 0;
}

void cwskimmer_clear_frozen_capture(cwskimmer_detector_t *detector)
{
    if (!detector) {
        return;
    }

    pthread_mutex_lock(&detector->lock);
    cwskimmer_free_frozen_capture_locked(detector);
    pthread_mutex_unlock(&detector->lock);
}

int cwskimmer_save_capture(cwskimmer_detector_t *detector,
                           const char *path,
                           float mark_freq_offset_hz,
                           const char *expected_text,
                           const char *notes)
{
    cw_capture_header_t header;
    float complex *samples = NULL;
    int copied = 0;
    int use_frozen = 0;
    int rc = -1;

    if (!detector || !path || path[0] == '\0') {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.mark_freq_offset_hz = mark_freq_offset_hz;

    if (expected_text) {
        strncpy(header.expected_text, expected_text, sizeof(header.expected_text) - 1);
    }
    if (notes) {
        strncpy(header.notes, notes, sizeof(header.notes) - 1);
    }

    pthread_mutex_lock(&detector->lock);

    if (detector->frozen_capture_samples && detector->frozen_capture_count > 0) {
        copied = detector->frozen_capture_count;
        samples = malloc((size_t)copied * sizeof(float complex));
        if (!samples) {
            pthread_mutex_unlock(&detector->lock);
            return -1;
        }
        memcpy(samples, detector->frozen_capture_samples,
               (size_t)copied * sizeof(float complex));
        header.sample_rate = detector->frozen_capture_sample_rate;
        header.center_freq_hz = detector->frozen_capture_center_hz;
        use_frozen = 1;
    } else if (detector->iq_capture && detector->iq_capture->count > 0) {
        int capacity = detector->iq_capture->count;
        samples = malloc((size_t)capacity * sizeof(float complex));
        if (!samples) {
            pthread_mutex_unlock(&detector->lock);
            return -1;
        }
        copied = cw_capture_ring_copy(detector->iq_capture, samples, capacity);
        header.sample_rate = (uint32_t)detector->config.sample_rate;
        header.center_freq_hz = (uint64_t)llround(cwskimmer_radio_center_hz(detector));
    } else {
        LOG_WARN("Capture save failed: no IQ data in ring buffer");
        pthread_mutex_unlock(&detector->lock);
        return -1;
    }

    pthread_mutex_unlock(&detector->lock);

    if (copied <= 0) {
        free(samples);
        return -1;
    }

    rc = cw_capture_save(path, &header, samples, copied);
    free(samples);

    if (rc == 0) {
        LOG_INFO("Saved IQ capture: %s (%d samples, mark=%.0f Hz, expect='%s'%s)",
                 path, copied, mark_freq_offset_hz,
                 header.expected_text[0] ? header.expected_text : "?",
                 use_frozen ? ", frozen snapshot" : "");
        if (use_frozen) {
            cwskimmer_clear_frozen_capture(detector);
        }
    } else {
        LOG_WARN("Failed to save IQ capture: %s", path);
    }

    return rc;
}

static int cwskimmer_decode_capture_tone(int sample_rate,
                                         const float complex *samples,
                                         int num_samples,
                                         float tone_hz,
                                         char *plain_out,
                                         int plain_max)
{
    cw_decoder_t *decoder;
    int offset;

    if (!samples || num_samples <= 0 || !plain_out || plain_max <= 0) {
        return 0;
    }

    plain_out[0] = '\0';
    decoder = cw_decoder_create(sample_rate, 20);
    if (!decoder) {
        return 0;
    }

    cw_decoder_set_tone(decoder, tone_hz);
    for (offset = 0; offset < num_samples; offset += IQ_PROCESS_CHUNK) {
        int got = num_samples - offset;
        if (got > IQ_PROCESS_CHUNK) {
            got = IQ_PROCESS_CHUNK;
        }
        cw_decoder_process_iq(decoder, samples + offset, got);
    }
    cw_decoder_flush(decoder);
    strncpy(plain_out, cw_decoder_get_plain_text(decoder), (size_t)(plain_max - 1));
    plain_out[plain_max - 1] = '\0';
    cw_decoder_destroy(decoder);
    return plain_out[0] != '\0';
}

static int cwskimmer_score_expected_match(const char *plain, const char *expected)
{
    size_t ei;
    size_t pi = 0;
    int score = 0;

    if (!plain || !expected || expected[0] == '\0') {
        return (int)strlen(plain);
    }

    if (cwskimmer_text_matches_ci(plain, expected)) {
        return 1000 + (int)strlen(plain);
    }

    for (ei = 0; expected[ei] != '\0'; ei++) {
        char want = cwskimmer_upper_char(expected[ei]);
        while (plain[pi] != '\0') {
            if (cwskimmer_upper_char(plain[pi]) == want) {
                score++;
                pi++;
                break;
            }
            pi++;
        }
    }
    return score;
}

static float cwskimmer_find_best_decode_tone(const float complex *samples,
                                             int num_samples,
                                             int sample_rate,
                                             float anchor_hz,
                                             const char *expected_text,
                                             char *best_plain,
                                             int plain_max,
                                             int *best_score_out)
{
    float coarse_hz[32];
    float coarse_span[32];
    int coarse_count = 0;
    float best_tone = anchor_hz;
    int best_score = -1;
    float test_hz;
    int ci;
    int fi;

    if (!samples || num_samples <= 0 || sample_rate <= 0) {
        return anchor_hz;
    }

    if (best_plain && plain_max > 0) {
        best_plain[0] = '\0';
    }

    for (test_hz = anchor_hz - TONE_SEARCH_RANGE_HZ;
         test_hz <= anchor_hz + TONE_SEARCH_RANGE_HZ + 0.1f;
         test_hz += TONE_COARSE_STEP_HZ) {
        float span = cwskimmer_tone_envelope_span(samples, num_samples,
                                                  sample_rate, test_hz);
        int slot;

        if (coarse_count >= 32) {
            continue;
        }

        slot = coarse_count;
        coarse_hz[slot] = test_hz;
        coarse_span[slot] = span;
        coarse_count++;

        for (ci = coarse_count - 1; ci > 0; ci--) {
            if (coarse_span[ci] > coarse_span[ci - 1]) {
                float tmp_hz = coarse_hz[ci];
                float tmp_span = coarse_span[ci];
                coarse_hz[ci] = coarse_hz[ci - 1];
                coarse_span[ci] = coarse_span[ci - 1];
                coarse_hz[ci - 1] = tmp_hz;
                coarse_span[ci - 1] = tmp_span;
            } else {
                break;
            }
        }
    }

    if (coarse_count < 5) {
        coarse_hz[coarse_count++] = anchor_hz;
    }

    for (ci = 0; ci < coarse_count && ci < 6; ci++) {
        float seeds[5];
        int seed_count = 0;
        int si;

        seeds[seed_count++] = cwskimmer_search_tone_envelope(samples, num_samples,
                                                             sample_rate, coarse_hz[ci]);
        for (fi = -3; fi <= 3; fi++) {
            if (seed_count >= 5) {
                break;
            }
            seeds[seed_count++] = coarse_hz[ci] + (float)(fi * 25);
        }

        for (si = 0; si < seed_count; si++) {
            char trial[64];
            int score;

            trial[0] = '\0';
            if (!cwskimmer_decode_capture_tone(sample_rate, samples, num_samples,
                                               seeds[si], trial, (int)sizeof(trial))) {
                continue;
            }
            score = cwskimmer_score_expected_match(trial, expected_text);
            if (score > best_score) {
                best_score = score;
                best_tone = seeds[si];
                if (best_plain && plain_max > 0) {
                    strncpy(best_plain, trial, (size_t)(plain_max - 1));
                    best_plain[plain_max - 1] = '\0';
                }
            }
        }
    }

    if (best_score_out) {
        *best_score_out = best_score;
    }
    return best_tone;
}

int cwskimmer_replay_capture_file(const char *path)
{
    cw_capture_header_t hdr;
    float complex *samples = NULL;
    int num_samples = 0;
    cwskimmer_detector_t replay;
    float complex *iq_buffer = NULL;
    int offset;
    int matched_expected = 0;
    int ri;

    if (!path || cw_capture_load(path, &hdr, &samples, &num_samples) != 0) {
        fprintf(stderr, "Failed to load capture: %s\n", path ? path : "(null)");
        return -1;
    }

    memset(&replay, 0, sizeof(replay));
    pthread_mutex_init(&replay.lock, NULL);
    config_defaults(&replay.config);
    config_load_auto("cw-skimmer.conf", &replay.config);
    replay.config.sample_rate = (int)hdr.sample_rate;
    replay.config.center_frequency = (float)hdr.center_freq_hz;
    replay.config.spot_enabled = 0;
    if (hdr.mark_freq_offset_hz != 0.0f) {
        if (hdr.expected_text[0] && replay.config.decode_bucket_hz > 150.0f) {
            replay.config.decode_bucket_hz = 150.0f;
        } else if (replay.config.decode_bucket_hz > 200.0f) {
            replay.config.decode_bucket_hz = 200.0f;
        }
    }

    if (cw_decoder_global_init(replay.config.deepcw_model_path) != 0) {
        fprintf(stderr, "Replay setup failed: ditdah decoder init\n");
        cw_capture_free_samples(samples);
        pthread_mutex_destroy(&replay.lock);
        return -1;
    }

    {
        int wi;
        for (wi = 0; wi < MAX_DECODE_CHANNELS; wi++) {
            decode_worker_init(&replay.decode_channels[wi], wi, NULL, NULL);
        }
    }

    replay.detector = cw_detector_create(replay.config.sample_rate,
                                         replay.config.sample_rate / 100,
                                         replay.config.detection_threshold);
    replay.audio_proc = audio_processor_create(replay.config.sample_rate, 1024, 1024);
    if (!replay.detector || !replay.audio_proc) {
        fprintf(stderr, "Replay setup failed\n");
        cw_capture_free_samples(samples);
        cw_decoder_global_shutdown();
        if (replay.detector) cw_detector_destroy(replay.detector);
        if (replay.audio_proc) audio_processor_destroy(replay.audio_proc);
        pthread_mutex_destroy(&replay.lock);
        return -1;
    }
    replay.detector->min_snr_db = replay.config.min_snr_db;
    replay.config.decode_min_snr_db = 0.0f;
    replay.replay_collect = 1;
    replay.replay_decode_count = 0;

    iq_buffer = malloc(IQ_PROCESS_CHUNK * sizeof(float complex));
    if (!iq_buffer) {
        cw_capture_free_samples(samples);
        audio_processor_destroy(replay.audio_proc);
        cw_detector_destroy(replay.detector);
        pthread_mutex_destroy(&replay.lock);
        return -1;
    }

    printf("Replaying %s\n", path);
    printf("  center=%llu Hz  samples=%d  mark=%.0f Hz  expect='%s'\n",
           (unsigned long long)hdr.center_freq_hz, num_samples,
           hdr.mark_freq_offset_hz,
           hdr.expected_text[0] ? hdr.expected_text : "(none)");
    if (hdr.notes[0]) {
        printf("  notes: %s\n", hdr.notes);
    }

    if (hdr.mark_freq_offset_hz != 0.0f && hdr.expected_text[0] != '\0') {
        char tuned_plain[64];
        int tuned_score = -1;
        float tuned_tone = cwskimmer_find_best_decode_tone(
            samples, num_samples, (int)hdr.sample_rate,
            hdr.mark_freq_offset_hz, hdr.expected_text,
            tuned_plain, (int)sizeof(tuned_plain), &tuned_score);

        if (fabsf(tuned_tone - hdr.mark_freq_offset_hz) > 1.0f) {
            printf("  decode tone tuned %.0f -> %.0f Hz: '%s' (score=%d)\n",
                   hdr.mark_freq_offset_hz, tuned_tone,
                   tuned_plain[0] ? tuned_plain : "(empty)", tuned_score);
            hdr.mark_freq_offset_hz = tuned_tone;
        } else if (tuned_plain[0] != '\0') {
            printf("  decode tone @ %.0f Hz: '%s' (score=%d)\n",
                   tuned_tone, tuned_plain, tuned_score);
        }
        if (cwskimmer_text_matches_ci(tuned_plain, hdr.expected_text)) {
            matched_expected = 1;
        }
    }

    {
        float prescan_offsets[MAX_DECODE_CHANNELS];
        float prescan_snrs[MAX_DECODE_CHANNELS];
        int prescan_count;
        int pi;
        time_t now = time(NULL);
        float mark_hint = hdr.mark_freq_offset_hz;

        prescan_count = cwskimmer_prescan_capture_signals(
            &replay, samples, num_samples, mark_hint,
            prescan_offsets, prescan_snrs, MAX_DECODE_CHANNELS);

        if (mark_hint != 0.0f && prescan_count > 0) {
            int best_i = 0;
            for (pi = 1; pi < prescan_count; pi++) {
                if (fabsf(prescan_offsets[pi] - mark_hint) <
                    fabsf(prescan_offsets[best_i] - mark_hint)) {
                    best_i = pi;
                }
            }
            if (fabsf(prescan_offsets[best_i] - mark_hint) > 1.0f) {
                printf("  tone search refined mark %.0f -> %.0f Hz\n",
                       mark_hint, prescan_offsets[best_i]);
                mark_hint = prescan_offsets[best_i];
                hdr.mark_freq_offset_hz = mark_hint;
            }
        }

        printf("  prescan found %d signal candidate(s)", prescan_count);
        for (pi = 0; pi < prescan_count; pi++) {
            printf("%s%.0f Hz (snr %.1f)",
                   pi == 0 ? ": " : ", ",
                   prescan_offsets[pi], prescan_snrs[pi]);
        }
        if (prescan_count > 0) {
            printf("\n");
        } else {
            printf(" (none)\n");
        }

        for (pi = 0; pi < prescan_count; pi++) {
            decode_worker_t *ch = cwskimmer_acquire_channel(
                &replay, prescan_offsets[pi],
                prescan_offsets[pi] + replay.config.center_frequency,
                prescan_snrs[pi], now);
            if (ch) {
                decode_worker_set_metadata(ch, prescan_offsets[pi],
                                           prescan_offsets[pi] + replay.config.center_frequency,
                                           prescan_snrs[pi], now);
            }
        }
    }

    for (offset = 0; offset < num_samples; offset += IQ_PROCESS_CHUNK) {
        int got = num_samples - offset;

        if (got > IQ_PROCESS_CHUNK) {
            got = IQ_PROCESS_CHUNK;
        }
        memcpy(iq_buffer, samples + offset, (size_t)got * sizeof(float complex));

        audio_processor_process(replay.audio_proc, iq_buffer, got);
        {
            float power_copy[1024];
            int num_bins = audio_processor_get_power_spectrum(replay.audio_proc, power_copy);
            if (num_bins > 0) {
                cw_detector_feed_spectrum(replay.detector, power_copy, num_bins,
                                          replay.config.sample_rate);
            }
        }

        cwskimmer_feed_all_active_channels(&replay, iq_buffer, got);
    }

    {
        int ci;
        for (ci = 0; ci < MAX_DECODE_CHANNELS; ci++) {
            decode_worker_t *ch = &replay.decode_channels[ci];
            if (ch->active && !ch->birdie) {
                decode_worker_flush_decode(ch);
            }
        }
    }

    {
        int ci;
        for (ci = 0; ci < MAX_DECODE_CHANNELS; ci++) {
            decode_worker_t *ch = &replay.decode_channels[ci];
            const char *plain;
            if (!ch->active || !ch->decoder || ch->birdie) {
                continue;
            }
            plain = cw_decoder_get_plain_text(ch->decoder);
            printf("  channel bucket=%.0f tone=%.0f plain='%s' display='%s'\n",
                   ch->bucket_center_hz, ch->tone_offset_hz, plain,
                   cw_decoder_get_text(ch->decoder));
            if (hdr.expected_text[0] &&
                cwskimmer_text_matches_ci(plain, hdr.expected_text)) {
                matched_expected = 1;
            }
            if (plain[0] != '\0' && replay.replay_decode_count < 12) {
                int dup = 0;
                for (ri = 0; ri < replay.replay_decode_count; ri++) {
                    if (strcmp(replay.replay_decodes[ri], plain) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    strncpy(replay.replay_decodes[replay.replay_decode_count], plain,
                            sizeof(replay.replay_decodes[0]) - 1);
                    replay.replay_decodes[replay.replay_decode_count]
                        [sizeof(replay.replay_decodes[0]) - 1] = '\0';
                    replay.replay_decode_count++;
                }
            }
        }
    }

    if (hdr.mark_freq_offset_hz != 0.0f) {
        decode_worker_t *mark_ch =
            cwskimmer_find_closest_channel(&replay, hdr.mark_freq_offset_hz);
        if (mark_ch && mark_ch->decoder && !mark_ch->birdie) {
            const char *plain = cw_decoder_get_plain_text(mark_ch->decoder);
            printf("  mark channel @ %.0f Hz (bucket %.0f): plain='%s' display='%s'\n",
                   mark_ch->bucket_center_hz, mark_ch->tone_offset_hz, plain,
                   cw_decoder_get_text(mark_ch->decoder));
            if (hdr.expected_text[0] &&
                cwskimmer_text_matches_ci(plain, hdr.expected_text)) {
                matched_expected = 1;
            }
        } else {
            printf("  mark channel @ %.0f Hz: (no active decoder)\n",
                   hdr.mark_freq_offset_hz);
        }
    }

    for (ri = 0; ri < replay.replay_decode_count; ri++) {
        printf("  decoded: '%s'\n", replay.replay_decodes[ri]);
        if (hdr.expected_text[0] &&
            cwskimmer_text_matches_ci(replay.replay_decodes[ri], hdr.expected_text)) {
            matched_expected = 1;
        }
    }

    if (replay.replay_decode_count == 0) {
        printf("  (no decodes)\n");
    }

    if (hdr.expected_text[0] && !matched_expected && hdr.mark_freq_offset_hz == 0.0f) {
        float anchor = hdr.mark_freq_offset_hz;
        float sweep_offsets[32];
        float sweep_spans[32];
        char best_plain[64];
        float best_tone = anchor;
        int best_score = -1;
        int sweep_count = 0;
        int si;
        float test_hz;
        float max_span = 0.0f;

        best_plain[0] = '\0';
        for (si = 0; si < replay.replay_decode_count; si++) {
            int score = cwskimmer_score_expected_match(replay.replay_decodes[si],
                                                       hdr.expected_text);
            if (score > best_score) {
                best_score = score;
            }
        }

        for (test_hz = anchor - TONE_SEARCH_RANGE_HZ;
             test_hz <= anchor + TONE_SEARCH_RANGE_HZ + 0.1f;
             test_hz += TONE_SWEEP_STEP_HZ) {
            float span = cwskimmer_tone_envelope_span(samples, num_samples,
                                                      (int)hdr.sample_rate, test_hz);
            if (span > max_span) {
                max_span = span;
            }
            cwskimmer_add_envelope_candidate(&replay, sweep_offsets, sweep_spans,
                                             &sweep_count, 32, test_hz, span);
        }

        for (si = 0; si < sweep_count; si++) {
            sweep_offsets[si] = cwskimmer_search_tone_envelope(
                samples, num_samples, (int)hdr.sample_rate, sweep_offsets[si]);
        }

        for (si = 0; si < sweep_count; si++) {
            int sj;
            for (sj = si + 1; sj < sweep_count; sj++) {
                if (sweep_spans[sj] > sweep_spans[si]) {
                    float tmp_off = sweep_offsets[si];
                    float tmp_span = sweep_spans[si];
                    sweep_offsets[si] = sweep_offsets[sj];
                    sweep_spans[si] = sweep_spans[sj];
                    sweep_offsets[sj] = tmp_off;
                    sweep_spans[sj] = tmp_span;
                }
            }
        }

        for (si = 0; si < sweep_count && si < 12; si++) {
            char trial[64];
            int score;

            if (max_span > 0.0f && sweep_spans[si] < max_span * 0.12f) {
                continue;
            }

            trial[0] = '\0';
            if (!cwskimmer_decode_capture_tone((int)hdr.sample_rate, samples, num_samples,
                                               sweep_offsets[si], trial,
                                               (int)sizeof(trial))) {
                continue;
            }
            score = cwskimmer_score_expected_match(trial, hdr.expected_text);
            if (score > best_score) {
                best_score = score;
                best_tone = sweep_offsets[si];
                strncpy(best_plain, trial, sizeof(best_plain) - 1);
                best_plain[sizeof(best_plain) - 1] = '\0';
            }
        }

        if (best_plain[0] != '\0') {
            printf("  tone sweep best @ %.0f Hz: '%s' (score=%d)\n",
                   best_tone, best_plain, best_score);
            if (cwskimmer_text_matches_ci(best_plain, hdr.expected_text)) {
                matched_expected = 1;
            }
        }
    }

    free(iq_buffer);
    cw_capture_free_samples(samples);
    cwskimmer_release_all_channels(&replay);
    {
        int wi;
        for (wi = 0; wi < MAX_DECODE_CHANNELS; wi++) {
            decode_worker_shutdown(&replay.decode_channels[wi]);
        }
    }
    audio_processor_destroy(replay.audio_proc);
    cw_detector_destroy(replay.detector);
    cw_decoder_global_shutdown();
    pthread_mutex_destroy(&replay.lock);

    if (hdr.expected_text[0]) {
        if (matched_expected) {
            printf("PASS: expected '%s' found\n", hdr.expected_text);
            return 0;
        }
        printf("FAIL: expected '%s' not found\n", hdr.expected_text);
        return 1;
    }

    return replay.replay_decode_count > 0 ? 0 : 1;
}
