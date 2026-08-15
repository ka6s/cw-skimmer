#include "decode_worker.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(_BSD_SOURCE) && !defined(_DEFAULT_SOURCE) && !defined(_XOPEN_SOURCE)
int usleep(unsigned int __useconds);
#endif

static void *decode_worker_thread(void *arg)
{
    decode_worker_t *worker = (decode_worker_t *)arg;
    float complex local_iq[DECODE_WORKER_IQ_CHUNK];
    int local_len = 0;

    while (1) {
        pthread_mutex_lock(&worker->lock);

        while (!worker->stop && worker->queue_count == 0 && worker->active) {
            pthread_cond_wait(&worker->work_cond, &worker->lock);
        }

        if (worker->stop) {
            pthread_mutex_unlock(&worker->lock);
            break;
        }

        if (!worker->active || !worker->decoder) {
            pthread_mutex_unlock(&worker->lock);
            continue;
        }

        if (worker->queue_count > 0) {
            int slot = worker->queue_read;
            local_len = worker->iq_queue_len[slot];
            if (local_len > DECODE_WORKER_IQ_CHUNK) {
                local_len = DECODE_WORKER_IQ_CHUNK;
            }
            memcpy(local_iq, worker->iq_queue[slot],
                   (size_t)local_len * sizeof(float complex));
            worker->queue_read = (worker->queue_read + 1) % DECODE_WORKER_QUEUE_DEPTH;
            worker->queue_count--;
        }

        pthread_mutex_unlock(&worker->lock);

        if (local_len <= 0 || !worker->decoder) {
            continue;
        }

        cw_decoder_set_tone(worker->decoder, worker->tone_offset_hz);
        cw_decoder_process_iq(worker->decoder, local_iq, local_len);
        cw_decoder_update(worker->decoder);

        if (cw_decoder_is_birdie(worker->decoder)) {
            decode_worker_result_cb cb;
            void *userdata;
            pthread_mutex_lock(&worker->lock);
            worker->birdie = 1;
            worker->active = 0;
            cb = worker->result_cb;
            userdata = worker->result_userdata;
            pthread_mutex_unlock(&worker->lock);
            LOG_DEBUG("Birdie detected @ %.0f Hz — ignoring steady carrier",
                      worker->frequency_hz);
            if (cb) {
                cb(worker, userdata);
            }
            continue;
        }

        pthread_mutex_lock(&worker->lock);
        if (worker->decoder && worker->result_cb) {
            char text[64];
            char partial[32];
            char new_letter = '\0';

            cw_decoder_get_display(worker->decoder, text, (int)sizeof(text),
                                   partial, (int)sizeof(partial), &new_letter);

            {
                const char *plain = cw_decoder_get_plain_text(worker->decoder);
                if (plain && strchr(plain, ' ')) {
                    cw_decoder_on_word_gap(worker->decoder);
                }
            }

            strncpy(worker->result_text, cw_decoder_get_plain_text(worker->decoder),
                    sizeof(worker->result_text) - 1);
            worker->result_text[sizeof(worker->result_text) - 1] = '\0';
            strncpy(worker->result_partial, partial, sizeof(worker->result_partial) - 1);
            worker->result_partial[sizeof(worker->result_partial) - 1] = '\0';
            worker->result_new_letter = new_letter;
            worker->result_callsign = cw_decoder_get_callsign(worker->decoder);
            worker->result_pending = 1;
            worker->result_cb(worker, worker->result_userdata);
            worker->result_pending = 0;
            cw_decoder_clear_display_dirty(worker->decoder);
        }
        if (worker->queue_count == 0) {
            pthread_cond_broadcast(&worker->work_cond);
        }
        pthread_mutex_unlock(&worker->lock);
    }

    return NULL;
}

void decode_worker_init(decode_worker_t *worker, int channel_index,
                        decode_worker_result_cb cb, void *userdata)
{
    if (!worker) return;
    memset(worker, 0, sizeof(*worker));
    worker->channel_index = channel_index;
    worker->result_cb = cb;
    worker->result_userdata = userdata;
    worker->thread_running = 0;
    pthread_mutex_init(&worker->lock, NULL);
    pthread_cond_init(&worker->work_cond, NULL);
}

void decode_worker_reset(decode_worker_t *worker)
{
    if (!worker) return;
    decode_worker_stop(worker);
    worker->birdie = 0;
    worker->bucket_center_hz = 0.0f;
    worker->tone_offset_hz = 0.0f;
    worker->frequency_hz = 0.0f;
    worker->peak_snr = 0.0f;
    worker->last_active = 0;
    worker->last_gui_text[0] = '\0';
    worker->last_gui_partial[0] = '\0';
    worker->last_completed_word[0] = '\0';
    worker->result_pending = 0;
    worker->result_text[0] = '\0';
    worker->result_partial[0] = '\0';
    worker->result_new_letter = '\0';
    memset(&worker->result_callsign, 0, sizeof(worker->result_callsign));
}

void decode_worker_shutdown(decode_worker_t *worker)
{
    if (!worker) return;
    decode_worker_stop(worker);
    pthread_mutex_destroy(&worker->lock);
    pthread_cond_destroy(&worker->work_cond);
}

int decode_worker_start(decode_worker_t *worker, int sample_rate, float tone_hz)
{
    if (!worker) return -1;

    /* Ensure any previous thread is fully joined before creating a new one */
    decode_worker_stop(worker);

    pthread_mutex_lock(&worker->lock);

    worker->decoder = cw_decoder_create(sample_rate, 20);
    if (!worker->decoder) {
        pthread_mutex_unlock(&worker->lock);
        return -1;
    }

    cw_decoder_set_tone(worker->decoder, tone_hz);
    worker->tone_offset_hz = tone_hz;
    worker->birdie = 0;
    worker->stop = 0;
    worker->active = 1;
    worker->queue_write = 0;
    worker->queue_read = 0;
    worker->queue_count = 0;
    worker->thread_running = 0;

    if (pthread_create(&worker->thread, NULL, decode_worker_thread, worker) != 0) {
        cw_decoder_destroy(worker->decoder);
        worker->decoder = NULL;
        worker->active = 0;
        pthread_mutex_unlock(&worker->lock);
        return -1;
    }
    worker->thread_running = 1;

    pthread_mutex_unlock(&worker->lock);
    return 0;
}

void decode_worker_stop(decode_worker_t *worker)
{
    pthread_t tid;
    int need_join = 0;

    if (!worker) {
        return;
    }

    pthread_mutex_lock(&worker->lock);
    worker->stop = 1;
    worker->active = 0;
    pthread_cond_broadcast(&worker->work_cond);
    if (worker->thread_running) {
        tid = worker->thread;
        worker->thread_running = 0;
        need_join = 1;
    }
    pthread_mutex_unlock(&worker->lock);

    /* Join at most once — double-join was the Start→Stop→Start segfault */
    if (need_join) {
        pthread_join(tid, NULL);
    }

    pthread_mutex_lock(&worker->lock);
    if (worker->decoder) {
        cw_decoder_destroy(worker->decoder);
        worker->decoder = NULL;
    }
    worker->stop = 0;
    worker->birdie = 0;
    worker->queue_write = 0;
    worker->queue_read = 0;
    worker->queue_count = 0;
    memset(&worker->thread, 0, sizeof(worker->thread));
    pthread_mutex_unlock(&worker->lock);
}

int decode_worker_is_running(const decode_worker_t *worker)
{
    return worker && worker->active && !worker->birdie;
}

void decode_worker_set_metadata(decode_worker_t *worker, float tone_hz,
                                float frequency_hz, float peak_snr, time_t now)
{
    if (!worker) return;
    pthread_mutex_lock(&worker->lock);
    worker->tone_offset_hz = tone_hz;
    worker->frequency_hz = frequency_hz;
    worker->peak_snr = peak_snr;
    worker->last_active = now;
    if (worker->decoder) {
        cw_decoder_set_tone(worker->decoder, tone_hz);
    }
    pthread_mutex_unlock(&worker->lock);
}

void decode_worker_wait_idle(decode_worker_t *worker)
{
    if (!worker) return;

    for (;;) {
        int pending;
        pthread_mutex_lock(&worker->lock);
        pending = worker->queue_count;
        pthread_mutex_unlock(&worker->lock);
        if (pending <= 0) {
            break;
        }
        usleep(2000);
    }
}

void decode_worker_flush_decode(decode_worker_t *worker)
{
    if (!worker) return;
    decode_worker_wait_idle(worker);
    pthread_mutex_lock(&worker->lock);
    if (worker->decoder) {
        cw_decoder_flush(worker->decoder);
        cw_decoder_on_word_gap(worker->decoder);
    }
    pthread_mutex_unlock(&worker->lock);
}

int decode_worker_submit_iq(decode_worker_t *worker,
                            const float complex *iq_samples,
                            int count)
{
    int slot;
    int copy_n;

    if (!worker || !iq_samples || count <= 0) {
        return -1;
    }

    pthread_mutex_lock(&worker->lock);
    if (!worker->active || worker->birdie || worker->queue_count >= DECODE_WORKER_QUEUE_DEPTH) {
        pthread_mutex_unlock(&worker->lock);
        return -1;
    }

    slot = worker->queue_write;
    copy_n = count > DECODE_WORKER_IQ_CHUNK ? DECODE_WORKER_IQ_CHUNK : count;
    memcpy(worker->iq_queue[slot], iq_samples, (size_t)copy_n * sizeof(float complex));
    worker->iq_queue_len[slot] = copy_n;
    worker->queue_write = (worker->queue_write + 1) % DECODE_WORKER_QUEUE_DEPTH;
    worker->queue_count++;
    pthread_cond_signal(&worker->work_cond);
    pthread_mutex_unlock(&worker->lock);
    return 0;
}