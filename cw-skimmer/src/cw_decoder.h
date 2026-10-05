#ifndef CW_DECODER_H
#define CW_DECODER_H

#include <stdint.h>
#include <complex.h>

typedef struct {
    int dit_length_ms;
    int dah_length_ms;
    int char_space_ms;
    int word_space_ms;
} morse_timing_t;

typedef struct {
    char callsign[16];
    float confidence;
    int valid;
} decoded_callsign_t;

typedef struct cw_decoder cw_decoder_t;

int cw_decoder_global_init(void);
void cw_decoder_global_shutdown(void);

cw_decoder_t *cw_decoder_create(int sample_rate, int wpm);

void cw_decoder_set_tone(cw_decoder_t *decoder, float tone_hz);

void cw_decoder_process_iq(cw_decoder_t *decoder,
                           const float complex *iq_samples,
                           int count);

void cw_decoder_process_keying(cw_decoder_t *decoder, const uint8_t *keying_data, int count);

void cw_decoder_add_keying(cw_decoder_t *decoder, int key_down, int num_samples);

void cw_decoder_add_element(cw_decoder_t *decoder, char element);

void cw_decoder_on_char_gap(cw_decoder_t *decoder);

void cw_decoder_on_word_gap(cw_decoder_t *decoder);

void cw_decoder_set_timing(cw_decoder_t *decoder, float dit_samples,
                           float char_space_samples, float word_space_samples);

decoded_callsign_t cw_decoder_get_callsign(cw_decoder_t *decoder);

const char *cw_decoder_get_symbols(cw_decoder_t *decoder);

const char *cw_decoder_get_text(cw_decoder_t *decoder);

const char *cw_decoder_get_plain_text(cw_decoder_t *decoder);

int cw_decoder_get_display(cw_decoder_t *decoder, char *text_out, int text_max,
                           char *partial_out, int partial_max,
                           char *new_letter_out);

void cw_decoder_clear_display_dirty(cw_decoder_t *decoder);

void cw_decoder_reset(cw_decoder_t *decoder);

void cw_decoder_destroy(cw_decoder_t *decoder);

void cw_decoder_update_wpm(cw_decoder_t *decoder, int measured_wpm);

int cw_decoder_get_wpm(const cw_decoder_t *decoder);

void cw_decoder_flush(cw_decoder_t *decoder);

/* Run ditdah decode pipeline on buffered audio; returns 1 if text changed. */
int cw_decoder_update(cw_decoder_t *decoder);

/* True when a persistent carrier shows no keying (passband birdie). */
int cw_decoder_is_birdie(const cw_decoder_t *decoder);

int cw_decoder_has_keying(const cw_decoder_t *decoder);

#endif