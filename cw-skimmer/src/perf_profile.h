#ifndef PERF_PROFILE_H
#define PERF_PROFILE_H

#define _POSIX_C_SOURCE 199309L

#include <stddef.h>
#include <time.h>

typedef enum {
    PERF_LOOP_TOTAL = 0,
    PERF_WS_SERVICE,
    PERF_TCI_READ,
    PERF_IQ_BLOCK,
    PERF_DETECTOR,
    PERF_AUDIO_PROC,
    PERF_PEAKS_SIGNALS,
    PERF_DECODE,
    PERF_SPECTRUM_CB,
    PERF_LOOP_SLEEP,
    PERF_SPECTRUM_GAP,
    PERF_STAGE_COUNT
} perf_stage_t;

void perf_reset(void);
void perf_begin(perf_stage_t stage);
void perf_end(perf_stage_t stage);
void perf_note_value(perf_stage_t stage, double value);
void perf_report_if_due(double interval_sec);
void perf_mark_now(struct timespec *ts);
double perf_elapsed_ms(const struct timespec *start, const struct timespec *end);
const char *perf_stage_name(perf_stage_t stage);

#endif