#ifndef DEEPCW_ENGINE_H
#define DEEPCW_ENGINE_H

#include <stdint.h>

/* DeepCW neural decoder (ported from web-deep-cw-decoder / deepcw-engine). */

typedef struct deepcw_engine deepcw_engine_t;

typedef struct {
    int sample_rate;
    int fft_length;
    int hop_length;
    float min_freq_hz;
    float max_freq_hz;
    int freq_bins;
    int blank_index;
    int num_classes;
    char input_name[64];
    char output_name[64];
    char vocabulary[48];
} deepcw_metadata_t;

int deepcw_engine_global_init(const char *model_path);
void deepcw_engine_global_shutdown(void);
int deepcw_engine_global_ready(void);

deepcw_engine_t *deepcw_engine_create(void);
void deepcw_engine_destroy(deepcw_engine_t *engine);

void deepcw_engine_reset(deepcw_engine_t *engine);
void deepcw_engine_set_tone_hz(deepcw_engine_t *engine, float tone_hz);

/**
 * Feed IQ and maybe run CTC inference (single-stream path).
 * For multi-stream GUI use feed_iq_no_infer + try_infer to avoid
 * stacking ONNX runs on the UI thread.
 */
int deepcw_engine_feed_iq(deepcw_engine_t *engine,
                          const float *iq_real,
                          const float *iq_imag,
                          int count,
                          int source_sample_rate);

/** Append/resample audio only — never runs ONNX. */
int deepcw_engine_feed_iq_no_infer(deepcw_engine_t *engine,
                                   const float *iq_real,
                                   const float *iq_imag,
                                   int count,
                                   int source_sample_rate);

/**
 * Run CTC if enough audio and interval elapsed.
 * @return 1 if inference ran, 0 if skipped, -1 on error
 */
int deepcw_engine_try_infer(deepcw_engine_t *engine);

int deepcw_engine_flush(deepcw_engine_t *engine);

const char *deepcw_engine_get_text(const deepcw_engine_t *engine);
const char *deepcw_engine_get_plain_text(const deepcw_engine_t *engine);
const char *deepcw_engine_get_pending(const deepcw_engine_t *engine);
int deepcw_engine_take_new_letter(deepcw_engine_t *engine, char *out);
int deepcw_engine_is_dirty(const deepcw_engine_t *engine);
void deepcw_engine_clear_dirty(deepcw_engine_t *engine);

/** Diagnostics: buffered model-rate audio length and samples since last infer. */
int deepcw_engine_audio_len(const deepcw_engine_t *engine);
int deepcw_engine_samples_since_infer(const deepcw_engine_t *engine);
int deepcw_engine_infer_interval_samples(const deepcw_engine_t *engine);

const deepcw_metadata_t *deepcw_engine_get_metadata(void);

#endif