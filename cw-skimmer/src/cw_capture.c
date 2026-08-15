#include "cw_capture.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int ensure_parent_dir(const char *path)
{
    char dir[4096];
    size_t len;
    size_t i;

    if (!path) {
        return -1;
    }

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    len = strlen(dir);
    if (len == 0) {
        return -1;
    }

    for (i = len; i > 0; --i) {
        if (dir[i - 1] == '/') {
            dir[i - 1] = '\0';
            break;
        }
    }

    if (dir[0] == '\0') {
        return 0;
    }

    if (mkdir(dir, 0755) == 0 || errno == EEXIST) {
        return 0;
    }

    return -1;
}

cw_capture_ring_t *cw_capture_ring_create(int sample_rate, int seconds)
{
    cw_capture_ring_t *ring;
    int capacity;

    if (sample_rate <= 0 || seconds <= 0) {
        return NULL;
    }

    capacity = sample_rate * seconds;
    ring = calloc(1, sizeof(*ring));
    if (!ring) {
        return NULL;
    }

    ring->samples = calloc((size_t)capacity, sizeof(float complex));
    if (!ring->samples) {
        free(ring);
        return NULL;
    }

    ring->capacity = capacity;
    ring->sample_rate = sample_rate;
    return ring;
}

void cw_capture_ring_destroy(cw_capture_ring_t *ring)
{
    if (!ring) {
        return;
    }
    free(ring->samples);
    free(ring);
}

void cw_capture_ring_write(cw_capture_ring_t *ring, const float complex *samples, int count)
{
    int i;

    if (!ring || !samples || count <= 0) {
        return;
    }

    for (i = 0; i < count; ++i) {
        ring->samples[ring->write_pos] = samples[i];
        ring->write_pos++;
        if (ring->write_pos >= ring->capacity) {
            ring->write_pos = 0;
        }
        if (ring->count < ring->capacity) {
            ring->count++;
        }
    }
}

int cw_capture_ring_copy(cw_capture_ring_t *ring, float complex *out, int out_max)
{
    int start;
    int i;

    if (!ring || !out || out_max <= 0) {
        return 0;
    }

    if (ring->count <= 0) {
        return 0;
    }

    if (ring->count < out_max) {
        out_max = ring->count;
    }

    start = ring->write_pos - ring->count;
    if (start < 0) {
        start += ring->capacity;
    }

    for (i = 0; i < out_max; ++i) {
        int idx = start + i;
        if (idx >= ring->capacity) {
            idx -= ring->capacity;
        }
        out[i] = ring->samples[idx];
    }

    return out_max;
}

int cw_capture_save(const char *path, const cw_capture_header_t *header,
                    const float complex *samples, int num_samples)
{
    FILE *fp;
    cw_capture_header_t hdr;
    size_t written;

    if (!path || !header || !samples || num_samples <= 0) {
        return -1;
    }

    if (ensure_parent_dir(path) != 0) {
        return -1;
    }

    fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }

    memcpy(&hdr, header, sizeof(hdr));
    memcpy(hdr.magic, CW_CAPTURE_MAGIC, 8);
    hdr.version = CW_CAPTURE_VERSION;
    hdr.num_samples = (uint64_t)num_samples;

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    written = fwrite(samples, sizeof(float complex), (size_t)num_samples, fp);
    fclose(fp);

    if ((int)written != num_samples) {
        return -1;
    }

    return 0;
}

int cw_capture_load(const char *path, cw_capture_header_t *header,
                    float complex **samples_out, int *num_samples_out)
{
    FILE *fp;
    cw_capture_header_t hdr;
    float complex *samples;
    size_t count;
    long file_size;
    int num_samples;

    if (!path || !header || !samples_out || !num_samples_out) {
        return -1;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    if (memcmp(hdr.magic, CW_CAPTURE_MAGIC, 8) != 0) {
        fclose(fp);
        return -1;
    }

    if (hdr.version != CW_CAPTURE_VERSION || hdr.num_samples == 0) {
        fclose(fp);
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    file_size = ftell(fp);
    if (file_size < (long)(sizeof(hdr) + sizeof(float complex))) {
        fclose(fp);
        return -1;
    }

    num_samples = (int)((file_size - (long)sizeof(hdr)) / (long)sizeof(float complex));
    if (num_samples <= 0) {
        fclose(fp);
        return -1;
    }

    samples = calloc((size_t)num_samples, sizeof(float complex));
    if (!samples) {
        fclose(fp);
        return -1;
    }

    if (fseek(fp, (long)sizeof(hdr), SEEK_SET) != 0) {
        free(samples);
        fclose(fp);
        return -1;
    }

    count = fread(samples, sizeof(float complex), (size_t)num_samples, fp);
    fclose(fp);

    if ((int)count != num_samples) {
        free(samples);
        return -1;
    }

    *header = hdr;
    *samples_out = samples;
    *num_samples_out = num_samples;
    return 0;
}

void cw_capture_free_samples(float complex *samples)
{
    free(samples);
}