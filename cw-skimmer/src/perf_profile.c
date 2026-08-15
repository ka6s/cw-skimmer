#define _POSIX_C_SOURCE 199309L

#include "perf_profile.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *name;
    double total_ms;
    double max_ms;
    double min_ms;
    double last_ms;
    unsigned long count;
    double value_sum;
    unsigned long value_count;
} perf_bucket_t;

static perf_bucket_t g_buckets[PERF_STAGE_COUNT];
static struct timespec g_active_start[PERF_STAGE_COUNT];
static unsigned char g_active[PERF_STAGE_COUNT];
static struct timespec g_last_report;
static unsigned char g_report_init;

static const char *k_names[PERF_STAGE_COUNT] = {
    "loop_total",
    "ws_service",
    "tci_read",
    "iq_block",
    "detector",
    "audio_proc",
    "peaks_signals",
    "decode",
    "spectrum_cb",
    "loop_sleep",
    "spectrum_gap"
};

static double timespec_to_ms(const struct timespec *ts)
{
    return (double)ts->tv_sec * 1000.0 + (double)ts->tv_nsec / 1000000.0;
}

static double timespec_diff_ms(const struct timespec *start, const struct timespec *end)
{
    struct timespec diff = *end;
    diff.tv_sec -= start->tv_sec;
    diff.tv_nsec -= start->tv_nsec;
    if (diff.tv_nsec < 0) {
        diff.tv_sec -= 1;
        diff.tv_nsec += 1000000000L;
    }
    return timespec_to_ms(&diff);
}

void perf_reset(void)
{
    memset(g_buckets, 0, sizeof(g_buckets));
    memset(g_active, 0, sizeof(g_active));
    memset(&g_last_report, 0, sizeof(g_last_report));
    g_report_init = 0;

    for (int i = 0; i < PERF_STAGE_COUNT; i++) {
        g_buckets[i].name = k_names[i];
        g_buckets[i].min_ms = 1e9;
    }
}

void perf_begin(perf_stage_t stage)
{
    if (stage < 0 || stage >= PERF_STAGE_COUNT) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &g_active_start[stage]);
    g_active[stage] = 1;
}

void perf_end(perf_stage_t stage)
{
    struct timespec now;
    double ms;

    if (stage < 0 || stage >= PERF_STAGE_COUNT || !g_active[stage]) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = timespec_diff_ms(&g_active_start[stage], &now);
    g_active[stage] = 0;

    perf_bucket_t *b = &g_buckets[stage];
    b->total_ms += ms;
    b->last_ms = ms;
    b->count++;
    if (ms > b->max_ms) {
        b->max_ms = ms;
    }
    if (ms < b->min_ms) {
        b->min_ms = ms;
    }
}

void perf_note_value(perf_stage_t stage, double value)
{
    if (stage < 0 || stage >= PERF_STAGE_COUNT) {
        return;
    }
    perf_bucket_t *b = &g_buckets[stage];
    b->value_sum += value;
    b->value_count++;
}

void perf_mark_now(struct timespec *ts)
{
    if (ts) {
        clock_gettime(CLOCK_MONOTONIC, ts);
    }
}

double perf_elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    if (!start || !end) {
        return 0.0;
    }
    return timespec_diff_ms(start, end);
}

const char *perf_stage_name(perf_stage_t stage)
{
    if (stage < 0 || stage >= PERF_STAGE_COUNT) {
        return "unknown";
    }
    return k_names[stage];
}

void perf_report_if_due(double interval_sec)
{
    struct timespec now;

    if (interval_sec <= 0.0) {
        interval_sec = 5.0;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!g_report_init) {
        g_last_report = now;
        g_report_init = 1;
        return;
    }

    if (timespec_diff_ms(&g_last_report, &now) < interval_sec * 1000.0) {
        return;
    }
    g_last_report = now;

    LOG_INFO("=== PERF REPORT (last %.0fs) ===", interval_sec);

    for (int i = 0; i < PERF_STAGE_COUNT; i++) {
        perf_bucket_t *b = &g_buckets[i];
        if (b->count == 0 && b->value_count == 0) {
            continue;
        }

        if (b->count > 0) {
            LOG_INFO("PERF %-14s: count=%lu avg=%.2f ms max=%.2f ms last=%.2f ms",
                     b->name, b->count,
                     b->total_ms / (double)b->count,
                     b->max_ms, b->last_ms);
        }

        if (b->value_count > 0) {
            LOG_INFO("PERF %-14s: note avg=%.1f (n=%lu)",
                     b->name, b->value_sum / (double)b->value_count, b->value_count);
        }
    }

    {
        perf_bucket_t *loop = &g_buckets[PERF_LOOP_TOTAL];
        perf_bucket_t *sleep = &g_buckets[PERF_LOOP_SLEEP];
        perf_bucket_t *gap = &g_buckets[PERF_SPECTRUM_GAP];
        perf_bucket_t *iq = &g_buckets[PERF_IQ_BLOCK];
        perf_bucket_t *det = &g_buckets[PERF_DETECTOR];
        perf_bucket_t *aud = &g_buckets[PERF_AUDIO_PROC];
        perf_bucket_t *gui = &g_buckets[PERF_SPECTRUM_CB];

        if (loop->count > 0) {
            double loop_avg = loop->total_ms / (double)loop->count;
            double sleep_avg = sleep->count > 0 ? sleep->total_ms / (double)sleep->count : 0.0;
            LOG_INFO("PERF summary: loop_avg=%.2f ms sleep_avg=%.2f ms spectrum_gap_avg=%.1f ms",
                     loop_avg, sleep_avg,
                     gap->value_count > 0 ? gap->value_sum / (double)gap->value_count : 0.0);
            LOG_INFO("PERF hotspots: iq_block_max=%.1f ms detector_max=%.1f ms audio_max=%.1f ms gui_cb_max=%.1f ms",
                     iq->max_ms, det->max_ms, aud->max_ms, gui->max_ms);
        }
    }

    LOG_INFO("=== END PERF REPORT ===");
}