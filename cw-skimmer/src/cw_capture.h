#ifndef CW_CAPTURE_H
#define CW_CAPTURE_H

#include <complex.h>
#include <stdint.h>

#define CW_CAPTURE_MAGIC "CWCAP001"
#define CW_CAPTURE_VERSION 1u
#define CW_CAPTURE_DEFAULT_SECONDS 20

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t sample_rate;
    uint64_t center_freq_hz;
    uint64_t num_samples;
    float mark_freq_offset_hz;
    char expected_text[64];
    char notes[256];
} cw_capture_header_t;

typedef struct {
    float complex *samples;
    int capacity;
    int write_pos;
    int count;
    int sample_rate;
} cw_capture_ring_t;

cw_capture_ring_t *cw_capture_ring_create(int sample_rate, int seconds);
void cw_capture_ring_destroy(cw_capture_ring_t *ring);
void cw_capture_ring_write(cw_capture_ring_t *ring, const float complex *samples, int count);
int cw_capture_ring_copy(cw_capture_ring_t *ring, float complex *out, int out_max);

int cw_capture_save(const char *path, const cw_capture_header_t *header,
                    const float complex *samples, int num_samples);

int cw_capture_load(const char *path, cw_capture_header_t *header,
                    float complex **samples_out, int *num_samples_out);

void cw_capture_free_samples(float complex *samples);

#endif