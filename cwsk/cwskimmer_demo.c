/* Synthetic on-channel CW through the lifted decoder.
   Build: gcc -O2 -o cwskimmer_demo cwskimmer_decode.c cwskimmer_demo.c -lm
*/
#include "cwskimmer_decode.h"

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
        {'Y', "-.--"},  {'Z', "--.."},  {'1', ".----"},{'2', "..---"},
        {'3', "...--"}, {'4', "....-"}, {'5', "....."},{'6', "-...."},
        {'7', "--..."}, {'8', "---.."}, {'9', "----."},{'0', "-----"},
        {'/', "-..-."}, {'?', "..--.."}, {'=', "-...-"},
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

/* Standard PARIS timing: dit = 1.2/wpm seconds. */
static int build_keyed(float *x, int cap, double fs, double pitch, double wpm,
                       const char *msg)
{
    double unit = 1.2 / wpm;
    double t = 0.35;          /* leading silence */
    double ramp = 0.002;
    int n, i, k;
    int pending = 0;
    int any = 0;
    double pulses[512][2];
    int np = 0;

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
    if (n > cap)
        n = cap;
    for (i = 0; i < n; i++) {
        double ts = (double)i / fs;
        double env = 0.0;
        int p;
        for (p = 0; p < np; p++) {
            double a = pulses[p][0], b = pulses[p][1];
            double u;
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
        /* Small noise so the key-up floor is not an exact zero. */
        {
            double noise = ((i * 1103515245u + 12345u) & 0xffff) / 65535.0;
            noise = (noise - 0.5) * 0.004;
            x[i] = (float)(env * sin(2.0 * M_PI * pitch * ts) + noise);
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

int main(void)
{
    CwDecoder *d;
    float *buf;
    char out[1024];
    char all[2048];
    int n, nw, got;
    double wpm;
    const double fs = 6000.0;
    const double pitch = 750.0;

    d = cw_decoder_create(fs, pitch, 120.0, 0);
    if (!d) {
        fprintf(stderr, "create failed\n");
        return 1;
    }
    wpm = cw_decoder_wpm(d);
    printf("seed WPM %.2f (tables imply ~23)\n", wpm);
    if (wpm < 20.0 || wpm > 26.0) {
        printf("FAIL seed WPM out of range\n");
        fails++;
    }

    buf = (float *)malloc(sizeof(float) * 6000 * 12);
    if (!buf)
        return 1;
    n = build_keyed(buf, 6000 * 12, fs, pitch, 18.0, "CQ DE K");
    all[0] = 0;
    nw = 0;
    while (nw < n) {
        int chunk = n - nw;
        if (chunk > 2048)
            chunk = 2048;
        got = cw_decoder_process(d, buf + nw, chunk, out, (int)sizeof out);
        if (got > 0 && (int)strlen(all) + got < (int)sizeof all - 1)
            strcat(all, out);
        nw += chunk;
    }
    got = cw_decoder_finish(d, out, (int)sizeof out);
    if (got > 0 && (int)strlen(all) + got < (int)sizeof all - 1)
        strcat(all, out);
    printf("decoded: \"%s\"\n", all);
    printf("stored:  \"%s\"\n", cw_decoder_text(d));
    expect_contains(cw_decoder_text(d), "CQ DE K", "CQ DE K at 18 WPM");

    cw_decoder_destroy(d);

    /* Off-rate input: 48 kHz linear-resampled onto the 6 kHz grid. */
    d = cw_decoder_create(48000.0, pitch, 120.0, 0);
    buf = (float *)realloc(buf, sizeof(float) * 48000 * 8);
    n = build_keyed(buf, 48000 * 8, 48000.0, pitch, 20.0, "E T");
    all[0] = 0;
    got = cw_decoder_process(d, buf, n, out, (int)sizeof out);
    if (got > 0)
        strncat(all, out, sizeof all - 1);
    got = cw_decoder_finish(d, out, (int)sizeof out);
    if (got > 0 && strlen(all) + (size_t)got < sizeof all - 1)
        strcat(all, out);
    printf("48 kHz: \"%s\" (stored \"%s\")\n", all, cw_decoder_text(d));
    expect_contains(cw_decoder_text(d), "ET", "E T at 48 kHz");
    cw_decoder_destroy(d);
    free(buf);

    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
