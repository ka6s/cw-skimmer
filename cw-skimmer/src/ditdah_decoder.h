#ifndef DITDAH_DECODER_H
#define DITDAH_DECODER_H

#include <complex.h>
#include <stdint.h>

#define DITDAH_TARGET_SAMPLE_RATE 12000

typedef struct ditdah_decoder ditdah_decoder_t;

ditdah_decoder_t *ditdah_decoder_create(int source_sample_rate, float tone_hz);
void ditdah_decoder_destroy(ditdah_decoder_t *decoder);

void ditdah_decoder_set_tone(ditdah_decoder_t *decoder, float tone_hz);
void ditdah_decoder_reset(ditdah_decoder_t *decoder);

/* Mix IQ at tone_hz, resample/filter, append to rolling buffer. */
void ditdah_decoder_feed_iq(ditdah_decoder_t *decoder,
                            const float complex *iq_samples,
                            int count);

/* Run decode pipeline on buffered audio; returns 1 if decoded text changed. */
int ditdah_decoder_update(ditdah_decoder_t *decoder);

void ditdah_decoder_flush(ditdah_decoder_t *decoder);

const char *ditdah_decoder_get_text(const ditdah_decoder_t *decoder);
const char *ditdah_decoder_get_plain_text(const ditdah_decoder_t *decoder);
const char *ditdah_decoder_get_pending(const ditdah_decoder_t *decoder);

int ditdah_decoder_take_new_letter(ditdah_decoder_t *decoder, char *out);
int ditdah_decoder_is_dirty(const ditdah_decoder_t *decoder);
void ditdah_decoder_clear_dirty(ditdah_decoder_t *decoder);

/* True when envelope analysis shows on/off keying (not a steady carrier). */
int ditdah_decoder_has_keying(const ditdah_decoder_t *decoder);

/* Persistent tone without keying — treat as passband birdie. */
int ditdah_decoder_is_birdie(const ditdah_decoder_t *decoder);

/* Seconds since creation / last reset. */
float ditdah_decoder_active_seconds(const ditdah_decoder_t *decoder);

int ditdah_decoder_get_wpm(const ditdah_decoder_t *decoder);

#endif