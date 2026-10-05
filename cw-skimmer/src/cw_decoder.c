#include "cw_decoder.h"
#include "ditdah_decoder.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

struct cw_decoder {
    ditdah_decoder_t *ditdah;
    int sample_rate;
    int wpm;
    float tone_hz;
    char callsign_buf[64];
    int callsign_ready;
};

int cw_decoder_global_init(void)
{
    LOG_INFO("ditdah Morse decoder initialized");
    return 0;
}

void cw_decoder_global_shutdown(void)
{
}

cw_decoder_t *cw_decoder_create(int sample_rate, int wpm)
{
    cw_decoder_t *decoder = calloc(1, sizeof(*decoder));

    if (!decoder) {
        return NULL;
    }

    decoder->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    decoder->wpm = wpm > 0 ? wpm : 20;
    decoder->tone_hz = 800.0f;

    decoder->ditdah = ditdah_decoder_create(decoder->sample_rate, decoder->tone_hz);
    if (!decoder->ditdah) {
        free(decoder);
        return NULL;
    }

    LOG_DEBUG("ditdah-backed decoder created: %d Hz input, tone %.0f Hz",
              decoder->sample_rate, decoder->tone_hz);
    return decoder;
}

void cw_decoder_set_tone(cw_decoder_t *decoder, float tone_hz)
{
    if (!decoder || !decoder->ditdah) {
        return;
    }
    decoder->tone_hz = tone_hz;
    ditdah_decoder_set_tone(decoder->ditdah, tone_hz);
}

void cw_decoder_process_iq(cw_decoder_t *decoder,
                           const float complex *iq_samples,
                           int count)
{
    if (!decoder || !decoder->ditdah || !iq_samples || count <= 0) {
        return;
    }
    ditdah_decoder_feed_iq(decoder->ditdah, iq_samples, count);
}

int cw_decoder_update(cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return 0;
    }
    return ditdah_decoder_update(decoder->ditdah);
}

int cw_decoder_is_birdie(const cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return 0;
    }
    return ditdah_decoder_is_birdie(decoder->ditdah);
}

int cw_decoder_has_keying(const cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return 0;
    }
    return ditdah_decoder_has_keying(decoder->ditdah);
}

void cw_decoder_process_keying(cw_decoder_t *decoder, const uint8_t *keying_data, int count)
{
    (void)decoder;
    (void)keying_data;
    (void)count;
}

void cw_decoder_add_keying(cw_decoder_t *decoder, int key_down, int num_samples)
{
    (void)decoder;
    (void)key_down;
    (void)num_samples;
}

void cw_decoder_add_element(cw_decoder_t *decoder, char element)
{
    (void)decoder;
    (void)element;
}

void cw_decoder_on_char_gap(cw_decoder_t *decoder)
{
    (void)decoder;
}

void cw_decoder_on_word_gap(cw_decoder_t *decoder)
{
    if (!decoder) {
        return;
    }
    decoder->callsign_ready = 1;
}

void cw_decoder_set_timing(cw_decoder_t *decoder, float dit_samples,
                           float char_space_samples, float word_space_samples)
{
    (void)dit_samples;
    (void)char_space_samples;
    (void)word_space_samples;
    (void)decoder;
}

static void cw_decoder_copy_word(const char *text, char *out, int out_max)
{
    const char *end;
    size_t len;

    if (!text || !out || out_max <= 0) {
        return;
    }

    while (*text == ' ') {
        text++;
    }
    end = strchr(text, ' ');
    if (!end) {
        end = text + strlen(text);
    }
    len = (size_t)(end - text);
    if (len >= (size_t)out_max) {
        len = (size_t)out_max - 1;
    }
    memcpy(out, text, len);
    out[len] = '\0';
}

decoded_callsign_t cw_decoder_get_callsign(cw_decoder_t *decoder)
{
    decoded_callsign_t result;

    memset(&result, 0, sizeof(result));
    if (!decoder || !decoder->ditdah) {
        return result;
    }

    if (decoder->callsign_ready) {
        const char *text = ditdah_decoder_get_plain_text(decoder->ditdah);
        cw_decoder_copy_word(text, result.callsign, (int)sizeof(result.callsign));
        if (result.callsign[0] != '\0') {
            result.confidence = 0.85f;
            result.valid = 1;
        }
        decoder->callsign_ready = 0;
        ditdah_decoder_clear_dirty(decoder->ditdah);
    }

    return result;
}

const char *cw_decoder_get_symbols(cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return "";
    }
    return ditdah_decoder_get_pending(decoder->ditdah);
}

const char *cw_decoder_get_text(cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return "";
    }
    return ditdah_decoder_get_text(decoder->ditdah);
}

const char *cw_decoder_get_plain_text(cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return "";
    }
    return ditdah_decoder_get_plain_text(decoder->ditdah);
}

int cw_decoder_get_display(cw_decoder_t *decoder, char *text_out, int text_max,
                           char *partial_out, int partial_max,
                           char *new_letter_out)
{
    char letter = '\0';

    if (!decoder || !decoder->ditdah) {
        return 0;
    }

    if (new_letter_out) {
        if (ditdah_decoder_take_new_letter(decoder->ditdah, &letter)) {
            *new_letter_out = letter;
        } else {
            *new_letter_out = '\0';
        }
    }

    if (text_out && text_max > 0) {
        strncpy(text_out, ditdah_decoder_get_text(decoder->ditdah), (size_t)(text_max - 1));
        text_out[text_max - 1] = '\0';
    }
    if (partial_out && partial_max > 0) {
        strncpy(partial_out, ditdah_decoder_get_pending(decoder->ditdah), (size_t)(partial_max - 1));
        partial_out[partial_max - 1] = '\0';
    }

    return ditdah_decoder_is_dirty(decoder->ditdah);
}

void cw_decoder_clear_display_dirty(cw_decoder_t *decoder)
{
    if (decoder && decoder->ditdah) {
        ditdah_decoder_clear_dirty(decoder->ditdah);
    }
}

void cw_decoder_reset(cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return;
    }
    ditdah_decoder_reset(decoder->ditdah);
    decoder->callsign_ready = 0;
    decoder->callsign_buf[0] = '\0';
}

void cw_decoder_update_wpm(cw_decoder_t *decoder, int measured_wpm)
{
    if (!decoder) {
        return;
    }
    if (measured_wpm > 0) {
        decoder->wpm = measured_wpm;
    }
}

int cw_decoder_get_wpm(const cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return 20;
    }
    return ditdah_decoder_get_wpm(decoder->ditdah);
}

void cw_decoder_flush(cw_decoder_t *decoder)
{
    if (!decoder || !decoder->ditdah) {
        return;
    }
    ditdah_decoder_flush(decoder->ditdah);
    cw_decoder_update(decoder);
}

void cw_decoder_destroy(cw_decoder_t *decoder)
{
    if (!decoder) {
        return;
    }
    if (decoder->ditdah) {
        ditdah_decoder_destroy(decoder->ditdah);
    }
    free(decoder);
    LOG_DEBUG("ditdah decoder destroyed");
}