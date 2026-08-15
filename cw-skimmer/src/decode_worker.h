#ifndef DECODE_WORKER_H
#define DECODE_WORKER_H

#include "cw_decoder.h"
#include <complex.h>
#include <pthread.h>
#include <time.h>

#define DECODE_WORKER_IQ_CHUNK 1024
#define DECODE_WORKER_QUEUE_DEPTH 8

typedef struct decode_worker decode_worker_t;

typedef void (*decode_worker_result_cb)(decode_worker_t *worker, void *userdata);

typedef struct decode_worker {
    int active;
    int stop;
    int birdie;
    int channel_index;

    float bucket_center_hz;   /* Stable grouping offset for this decode bucket */
    float tone_offset_hz;
    float frequency_hz;
    float peak_snr;
    time_t last_active;

    char last_gui_text[64];
    char last_gui_partial[32];
    char last_completed_word[64];

    cw_decoder_t *decoder;
    pthread_t thread;
    int thread_running;   /* 1 only while thread is joinable (never double-join) */
    pthread_mutex_t lock;
    pthread_cond_t work_cond;

    float complex iq_queue[DECODE_WORKER_QUEUE_DEPTH][DECODE_WORKER_IQ_CHUNK];
    int iq_queue_len[DECODE_WORKER_QUEUE_DEPTH];
    int queue_write;
    int queue_read;
    int queue_count;

    int result_pending;
    char result_text[64];
    char result_partial[32];
    char result_new_letter;
    decoded_callsign_t result_callsign;

    decode_worker_result_cb result_cb;
    void *result_userdata;
} decode_worker_t;

void decode_worker_init(decode_worker_t *worker, int channel_index,
                        decode_worker_result_cb cb, void *userdata);
void decode_worker_shutdown(decode_worker_t *worker);
void decode_worker_reset(decode_worker_t *worker);

int decode_worker_start(decode_worker_t *worker, int sample_rate, float tone_hz);
void decode_worker_stop(decode_worker_t *worker);

int decode_worker_is_running(const decode_worker_t *worker);

void decode_worker_set_metadata(decode_worker_t *worker, float tone_hz,
                                float frequency_hz, float peak_snr, time_t now);

/* Non-blocking: copies chunk into worker queue. Returns 0 if queue full. */
int decode_worker_submit_iq(decode_worker_t *worker,
                            const float complex *iq_samples,
                            int count);

/* Block until queued IQ chunks are processed (for offline replay). */
void decode_worker_wait_idle(decode_worker_t *worker);

void decode_worker_flush_decode(decode_worker_t *worker);

#endif