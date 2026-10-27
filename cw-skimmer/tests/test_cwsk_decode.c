/* Synthetic CW through the integrated channel decoder.
   gcc -O2 -I src -o /tmp/test_cwsk_decode tests/test_cwsk_decode.c src/cwsk_decoder.c -lm
*/
#define _DEFAULT_SOURCE
#include "cwsk_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *pattern_of(char c)
{
    static const struct { char c; const char *p; } tab[] = {
        {'A', ".-"},    {'B', "-..."},  {'C', "-.-."}, {'D', "-.."},
        {'E', "."},     {'F', "..-."},  {'G', "--."},  {'H', "...."},
        {'I', ".."},    {'J', ".---"},  {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"},    {'N', "-."},    {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},  {'T', "-"},
        {'U', "..-"},   {'V', "...-"},  {'W', ".--"},  {'X', "-..-"},
        {'Y', "-.--"},  {'Z', "--.."},
        {0, NULL}
    };
    int i;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (i = 0; tab[i].p; i++)
        if (tab[i].c == c)
            return tab[i].p;
    return NULL;
}

/* Real tone, or complex baseband when iq != 0. freq_hz is the true frequency. */
static int build_keyed(float *x, int cap, double fs, double freq_hz, double wpm,
                       const char *msg, int iq)
{
    double unit = 1.2 / wpm;
    double t = 0.35;
    double ramp = 0.002;
    int n, i, k, pending = 0, any = 0, np = 0;
    double pulses[512][2];

    for (k = 0; msg[k]; k++) {
        const char *pat;
        int e;
        if (msg[k] == ' ') {
            pending = 7;
            continue;
        }
        pat = pattern_of(msg[k]);
        if (!pat)
            continue;
        if (pending)
            t += pending * unit;
        else if (any)
            t += 3.0 * unit;
        pending = 3;
        any = 1;
        for (e = 0; pat[e]; e++) {
            double dur = (pat[e] == '-') ? 3.0 * unit : unit;
            if (np < 512) {
                pulses[np][0] = t;
                pulses[np][1] = t + dur;
                np++;
            }
            t += dur;
            if (pat[e + 1])
                t += unit;
        }
    }
    t += 0.8;
    n = (int)(t * fs);
    if (iq)
        n *= 2;
    if (n > cap)
        n = cap;
    for (i = 0; i < (iq ? n / 2 : n); i++) {
        double ts = (double)i / fs;
        double env = 0.0;
        int p;
        double noise = ((i * 1103515245u + 12345u) & 0xffff) / 65535.0;
        noise = (noise - 0.5) * 0.004;
        for (p = 0; p < np; p++) {
            double a = pulses[p][0], b = pulses[p][1], u;
            if (ts < a || ts >= b)
                continue;
            u = (ts - a) / ramp;
            if ((b - ts) / ramp < u)
                u = (b - ts) / ramp;
            if (u > 1.0)
                u = 1.0;
            if (u < 0.0)
                u = 0.0;
            env = 0.5 - 0.5 * cos(M_PI * u);
            break;
        }
        if (!iq) {
            x[i] = (float)(env * sin(2.0 * M_PI * freq_hz * ts) + noise);
        } else {
            x[2 * i] = (float)(env * cos(2.0 * M_PI * freq_hz * ts) + noise);
            x[2 * i + 1] = (float)(env * sin(2.0 * M_PI * freq_hz * ts));
        }
    }
    return n;
}

static int fails;

static void expect_contains(const char *got, const char *needle, const char *what)
{
    if (!got || !strstr(got, needle)) {
        printf("FAIL %s: got \"%s\", expected to contain \"%s\"\n",
               what, got ? got : "(null)", needle);
        fails++;
    } else {
        printf("ok   %s: \"%s\"\n", what, got);
    }
}

static void run_case(double fs, double freq, double pitch, double bandwidth, int iq,
                     double wpm, const char *msg, const char *needle, const char *what)
{
    CwskDecoder *d;
    float *buf;
    char out[1024];
    int n, cap, nw;
    d = cwsk_decoder_create(fs, pitch, bandwidth, iq);
    if (!d) {
        printf("FAIL %s: create\n", what);
        fails++;
        return;
    }
    cap = (int)(fs * 12) * (iq ? 2 : 1);
    buf = (float *)calloc((size_t)cap, sizeof(float));
    if (!buf) {
        cwsk_decoder_destroy(d);
        return;
    }
    n = build_keyed(buf, cap, fs, freq, wpm, msg, iq);
    for (nw = 0; nw < n; ) {
        int chunk = n - nw;
        if (chunk > 4096)
            chunk = 4096;
        cwsk_decoder_process(d, buf + nw, chunk, out, (int)sizeof out);
        nw += chunk;
    }
    cwsk_decoder_finish(d, out, (int)sizeof out);
    printf("%s stored \"%s\"\n", what, cwsk_decoder_text(d));
    expect_contains(cwsk_decoder_text(d), needle, what);
    cwsk_decoder_destroy(d);
    free(buf);
}

int main(void)
{
    /* Same real-audio case as the original cwsk demo. */
    run_case(48000.0, 750.0, 750.0, 120.0, 0, 20.0, "E T", "ET", "real 48 kHz");
    /* GUI audio path: Q is zero, decoder still runs in complex mode. */
    run_case(48000.0, 750.0, 750.0, 150.0, 1, 20.0, "E T", "ET", "audio as IQ");
    run_case(48000.0, 800.0, 800.0, 150.0, 1, 18.0, "CQ", "CQ", "IQ +800 Hz");
    run_case(48000.0, -600.0, -600.0, 150.0, 1, 18.0, "CQ", "CQ", "IQ -600 Hz");
    /* Waterfall draws true +800 Hz at offset -800. The pane passes -offset. */
    run_case(48000.0, 800.0, -(-800.0), 150.0, 1, 18.0, "DE", "DE", "mirror offset -800");
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
