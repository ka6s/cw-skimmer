#include "../src/cw_capture.h"
#include "../src/cwskimmer_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void synth_fill(float complex *samples, int count, int sample_rate,
                       float tone_hz, int wpm, int key_down, float amplitude)
{
    static double phase = 0.0;
    int i;
    float omega = 2.0f * (float)M_PI * tone_hz / (float)sample_rate;
    float amp = key_down ? amplitude : amplitude * 0.02f;

    for (i = 0; i < count; ++i) {
        float re = amp * cosf((float)phase);
        float im = amp * sinf((float)phase);
        samples[i] = re + im * I;
        phase += (double)omega;
        if (phase > 2.0 * M_PI) {
            phase -= 2.0 * M_PI;
        }
    }
}

static void synth_append(float complex *out, int *pos, int max_samples,
                         int sample_rate, float tone_hz, int wpm,
                         int key_down, float dit_units)
{
    int dit_samples = (int)(1.2f / (float)wpm * (float)sample_rate + 0.5f);
    int run = (int)(dit_units * (float)dit_samples + 0.5f);
    int got;

    if (*pos >= max_samples) {
        return;
    }
    if (*pos + run > max_samples) {
        run = max_samples - *pos;
    }
    got = run;
    synth_fill(out + *pos, got, sample_rate, tone_hz, wpm, key_down, 0.0012f);
    *pos += got;
}

static void synth_morse(float complex *out, int *pos, int max_samples,
                        int sample_rate, float tone_hz, int wpm,
                        const char *symbols)
{
    size_t i;

    for (i = 0; symbols[i] != '\0'; ++i) {
        if (symbols[i] == '.') {
            synth_append(out, pos, max_samples, sample_rate, tone_hz, wpm, 1, 1.0f);
            synth_append(out, pos, max_samples, sample_rate, tone_hz, wpm, 0, 1.0f);
        } else if (symbols[i] == '-') {
            synth_append(out, pos, max_samples, sample_rate, tone_hz, wpm, 1, 3.0f);
            synth_append(out, pos, max_samples, sample_rate, tone_hz, wpm, 0, 1.0f);
        } else if (symbols[i] == ' ') {
            synth_append(out, pos, max_samples, sample_rate, tone_hz, wpm, 0, 3.0f);
        }
    }
}

int main(void)
{
    const int sample_rate = 48000;
    const int seconds = 6;
    const int num_samples = sample_rate * seconds;
    const float tone_hz = -938.0f;
    const int wpm = 20;
    float complex *samples;
    cw_capture_header_t header;
    char path[256];
    int pos = 0;
    int rc;

    samples = calloc((size_t)num_samples, sizeof(float complex));
    if (!samples) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    synth_append(samples, &pos, num_samples, sample_rate, tone_hz, wpm, 0, 12.0f);
    synth_morse(samples, &pos, num_samples, sample_rate, tone_hz, wpm, "-.");
    synth_append(samples, &pos, num_samples, sample_rate, tone_hz, wpm, 0, 3.0f);
    synth_morse(samples, &pos, num_samples, sample_rate, tone_hz, wpm, ".....");
    synth_append(samples, &pos, num_samples, sample_rate, tone_hz, wpm, 0, 3.0f);
    synth_morse(samples, &pos, num_samples, sample_rate, tone_hz, wpm, "--");
    synth_append(samples, &pos, num_samples, sample_rate, tone_hz, wpm, 0, 3.0f);
    synth_morse(samples, &pos, num_samples, sample_rate, tone_hz, wpm, "..-.");
    synth_append(samples, &pos, num_samples, sample_rate, tone_hz, wpm, 0, 7.0f);

    memset(&header, 0, sizeof(header));
    header.sample_rate = (uint32_t)sample_rate;
    header.center_freq_hz = 14074000ULL;
    header.mark_freq_offset_hz = tone_hz;
    strncpy(header.expected_text, "N5MF", sizeof(header.expected_text) - 1);

    snprintf(path, sizeof(path), "/tmp/cwcap_synth_n5mf.cwcap");
    if (cw_capture_save(path, &header, samples, pos) != 0) {
        fprintf(stderr, "failed to save synthetic capture\n");
        free(samples);
        return 1;
    }

    printf("Saved synthetic capture: %s (%d samples)\n", path, pos);
    rc = cwskimmer_replay_capture_file(path);
    free(samples);

    if (rc != 0) {
        fprintf(stderr, "Synthetic replay failed\n");
        return 1;
    }

    printf("Synthetic N5MF replay PASS\n");
    return 0;
}