#ifndef CW_KEYING_DETECTOR_H
#define CW_KEYING_DETECTOR_H

#include <complex.h>
#include "cw_decoder.h"

typedef struct cw_keying_detector cw_keying_detector_t;

cw_keying_detector_t *cw_keying_detector_create(cw_decoder_t *decoder,
                                                int sample_rate,
                                                float tone_hz,
                                                int initial_wpm);

void cw_keying_detector_destroy(cw_keying_detector_t *kd);

void cw_keying_detector_set_tone(cw_keying_detector_t *kd, float tone_hz);

void cw_keying_detector_set_snr(cw_keying_detector_t *kd, float snr_db);

void cw_keying_detector_process_iq(cw_keying_detector_t *kd,
                                   const float complex *iq_samples,
                                   int count);

void cw_keying_detector_flush(cw_keying_detector_t *kd);

float cw_keying_detector_get_mean_dit(const cw_keying_detector_t *kd);

float cw_keying_detector_get_mean_dah(const cw_keying_detector_t *kd);

int cw_keying_detector_get_wpm_estimate(const cw_keying_detector_t *kd);

const char *cw_keying_detector_get_trace(const cw_keying_detector_t *kd);

#endif