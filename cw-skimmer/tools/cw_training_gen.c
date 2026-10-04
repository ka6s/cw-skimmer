/*
 * cw_training_gen — write CW training streams in the TCI format the skimmer
 * already parses (semicolon-terminated text, then 64-byte IQ frames).
 *
 * Each set is 40 seconds of 48 kHz complex baseband. Carriers in one file are
 * at least 2 kHz apart, inside the ±24 kHz band. Every tone is keyed for
 * nearly the whole file: a short lead-in, then dictionary English at 5 to 30
 * WPM until the last whole word that still fits before the closing half
 * second. Each tone's level moves on its own random path from barely
 * detectable (just above the noise on a 1024-point spectrum) up to full
 * strength. The exact text is the label:
 *   set_NN.txt              one signal per line, including end time
 *   transcripts.txt         the same labels, all sets
 *   decoder_labels.tsv      machine-readable copy for training
 *   cw_train:...;           the same text inside the TCI byte stream
 *
 * Build:  make training-gen
 * Run:    ./bin/cw-training-gen [output-dir]
 */

#include "tci_stream.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TRAIN_SAMPLE_RATE 48000
#define TRAIN_CENTER_HZ 14074000LL
#define TRAIN_SECONDS 40
#define TRAIN_FRAME_PAIRS 512
#define TRAIN_MAX_SIGS 8
#define TRAIN_MAX_ELS 512
#define TRAIN_RAMP_SAMPLES 192
#define TRAIN_REF_AMPLITUDE 0.20f
/* 0 dB is full strength: carrier amplitude TRAIN_REF_AMPLITUDE. */
#define TRAIN_FULL_DB 0.0f
/* Center-to-center. Matches one decoder per 2 kHz window. */
#define TRAIN_MIN_SEP_HZ 2000.0f
#define TRAIN_LEVEL_PARTS 3
/* cw_train text is scanned with %127[^;] into a 128-byte field. */
#define TRAIN_TEXT_MAX 127
/* Every tone starts here and runs until the file's closing margin. */
#define TRAIN_LEAD_S 0.25f

typedef char tci_hdr_size_ok[(sizeof(tci_stream_header_t) == TCI_STREAM_HEADER_SIZE) ? 1 : -1];

typedef struct {
    float offset_hz;
    int wpm;
    const char *opening;
} train_tone_t;

typedef struct {
    float offset_hz;
    float atten_db;   /* midpoint of the random level swing, dB vs full strength */
    float fade_db;    /* half of that swing, so the level stays in atten ± fade */
    int wpm;
    float start_s;
    const char *text;
} train_sig_t;

typedef struct {
    const char *id;
    float noise_sigma;
    const train_tone_t *tones;
    int nsigs;
} train_set_t;

/* Slow random level. Evaluated as a sum of sines, then stretched so the
 * 40 s file reaches both barely detectable and full strength. */
typedef struct {
    float freq_hz[TRAIN_LEVEL_PARTS];
    float phase[TRAIN_LEVEL_PARTS];
    float weight[TRAIN_LEVEL_PARTS];
    float raw_min;
    float raw_max;
    float barely_db;
    float full_db;
} level_curve_t;

typedef struct {
    int start;
    int len;
} keyed_el_t;

static const train_tone_t k_set01[] = {
    { -20000.0f,  5, "THE CAT" },
    { -11000.0f, 12, "THE QUICK FOX" },
    {  -2000.0f, 18, "SEND A CLEAR WORD" },
    {   7000.0f, 24, "THE BROWN FOX JUMPS OVER" },
    {  16000.0f, 30, "PLEASE READ THIS GOOD WORD" },
};

static const train_tone_t k_set02[] = {
    { -18500.0f,  6, "RED HEN" },
    {  -9000.0f, 10, "BLUE BIRD" },
    {   1000.0f, 16, "A FAST RED FOX" },
    {  11000.0f, 22, "THE SMALL FISH" },
    {  21000.0f, 28, "PLEASE COPY THIS CLEAR WORD" },
};

static const train_tone_t k_set03[] = {
    { -22000.0f,  7, "BIG DOG" },
    { -14000.0f, 11, "GREEN TREE" },
    {  -5000.0f, 15, "OPEN THE PATH" },
    {   4000.0f, 19, "A WARM SUN DAY" },
    {  13000.0f, 26, "THE HORSE IS HOME" },
    {  22000.0f, 30, "SEND THE FULL TRUE COPY" },
};

static const train_tone_t k_set04[] = {
    { -16000.0f,  5, "A HEN" },
    {  -6000.0f, 14, "GOOD DAY TO YOU" },
    {   3000.0f, 21, "THE RIVER IS NEAR" },
    {  15000.0f, 29, "PLEASE SEND A SLOW CLEAR COPY" },
};

static const train_tone_t k_set05[] = {
    { -19000.0f,  8, "SOFT RAIN" },
    { -12000.0f, 13, "THE OLD BARN" },
    {  -3000.0f, 17, "A BIG RED BARN" },
    {   6000.0f, 23, "THE NIGHT WIND IS COLD" },
    {  17000.0f, 30, "THE QUICK BROWN FOX JUMPS OVER" },
};

static const train_tone_t k_set06[] = {
    { -21000.0f,  6, "GOOD LIGHT" },
    { -13000.0f,  9, "FAR HILL" },
    {  -4000.0f, 15, "READ THE WORD" },
    {   5000.0f, 20, "A SMALL BOAT ON THE LAKE" },
    {  14000.0f, 27, "THE SHEEP AND THE GOAT" },
    {  21000.0f, 30, "PLEASE COPY THE TRUE WORD" },
};

static const train_tone_t k_set07[] = {
    { -17500.0f,  5, "A CAT" },
    {  -7500.0f, 10, "COPY THIS" },
    {   2500.0f, 18, "HEAR THE LOUD BIRD" },
    {  12500.0f, 25, "THE LAZY FOX AND THE HEN" },
    {  20000.0f, 30, "SEND A CLEAR COPY OF THIS WORD" },
};

static const train_tone_t k_set08[] = {
    { -15000.0f,  9, "DARK NIGHT" },
    {  -5000.0f, 16, "WATER IS COLD" },
    {   4500.0f, 22, "THE TRAIN IS NEAR" },
    {  15500.0f, 28, "PLEASE READ THE FIRST WORD" },
};

static const train_tone_t k_set09[] = {
    { -20500.0f,  7, "LEFT PATH" },
    { -10500.0f, 14, "THE HOUSE IS WARM" },
    {  -1500.0f, 19, "A GREEN FIELD OF SHEEP" },
    {   8500.0f, 26, "THE BOAT IS ON THE LAKE" },
    {  18500.0f, 30, "THE QUICK BROWN FOX IS FAST" },
};

static const train_tone_t k_set10[] = {
    { -19500.0f,  5, "A DOG" },
    {  -8500.0f, 11, "NORTH STAR" },
    {   1500.0f, 17, "CLOSE THE PATH" },
    {   9500.0f, 24, "MANY FISH IN THE RIVER" },
    {  19000.0f, 29, "SEND THIS TRUE WORD AGAIN" },
};

static const train_set_t k_sets[] = {
    { "01", 0.00055f, k_set01, 5 },
    { "02", 0.00070f, k_set02, 5 },
    { "03", 0.00090f, k_set03, 6 },
    { "04", 0.00050f, k_set04, 4 },
    { "05", 0.00110f, k_set05, 5 },
    { "06", 0.00080f, k_set06, 6 },
    { "07", 0.00130f, k_set07, 5 },
    { "08", 0.00065f, k_set08, 4 },
    { "09", 0.00100f, k_set09, 5 },
    { "10", 0.00145f, k_set10, 5 },
};

/* Every keyed token must be one of these English words. */
static const char *k_words[] = {
    "A", "AGAIN", "AND", "BARN", "BIG", "BIRD", "BLUE", "BOAT", "BROWN",
    "CAT", "CLEAR", "CLOSE", "COLD", "COPY", "DARK", "DAY", "DOG", "FAR",
    "FAST", "FIELD", "FIRST", "FISH", "FOX", "FULL", "GOAT", "GOOD", "GREEN",
    "HEAR", "HEN", "HILL", "HOME", "HORSE", "HOUSE", "IN", "IS", "JUMPS",
    "LAKE", "LAZY", "LEFT", "LIGHT", "LOUD", "MANY", "NEAR", "NIGHT", "NORTH",
    "OF", "OLD", "ON", "OPEN", "OVER", "PATH", "PLEASE", "QUICK", "RAIN",
    "READ", "RED", "RIVER", "SEND", "SHEEP", "SLOW", "SMALL", "SOFT", "STAR",
    "SUN", "THE", "THIS", "TO", "TRAIN", "TREE", "TRUE", "WARM", "WATER",
    "WIND", "WORD", "YOU",
};

static int is_dictionary_word(const char *word) {
    size_t i;
    for (i = 0; i < sizeof(k_words) / sizeof(k_words[0]); ++i) {
        if (strcmp(k_words[i], word) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Letters and single spaces only, each word in the dictionary, speed 5..30. */
static int validate_signal(const train_sig_t *sig) {
    char word[64];
    int words = 0;
    int n = 0;
    const char *p;

    if (!sig || !sig->text || !sig->text[0]) {
        fprintf(stderr, "empty training text\n");
        return -1;
    }
    if (sig->wpm < 5 || sig->wpm > 30) {
        fprintf(stderr, "WPM %d is outside 5..30 for \"%s\"\n", sig->wpm, sig->text);
        return -1;
    }
    if (sig->text[0] == ' ' || sig->text[strlen(sig->text) - 1] == ' ') {
        fprintf(stderr, "leading or trailing space in \"%s\"\n", sig->text);
        return -1;
    }
    for (p = sig->text; *p; ++p) {
        if (*p == ' ') {
            if (p[1] == ' ') {
                fprintf(stderr, "double space in \"%s\"\n", sig->text);
                return -1;
            }
            continue;
        }
        if (*p < 'A' || *p > 'Z') {
            fprintf(stderr, "non-word character '%c' in \"%s\"\n", *p, sig->text);
            return -1;
        }
    }
    for (p = sig->text; ; ++p) {
        if (*p == ' ' || *p == '\0') {
            word[n] = '\0';
            if (n == 0 || !is_dictionary_word(word)) {
                fprintf(stderr, "\"%s\" is not a dictionary word (in \"%s\")\n",
                        word, sig->text);
                return -1;
            }
            words++;
            n = 0;
            if (*p == '\0') {
                break;
            }
            continue;
        }
        if (n + 1 >= (int)sizeof(word)) {
            fprintf(stderr, "word too long in \"%s\"\n", sig->text);
            return -1;
        }
        word[n++] = *p;
    }
    if (words < 1) {
        fprintf(stderr, "no words in \"%s\"\n", sig->text);
        return -1;
    }
    return 0;
}

static uint32_t g_rng = 1u;

static uint32_t rng_next(void) {
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0) {
        x = 0xA5A5A5A5u;
    }
    g_rng = x;
    return x;
}

static float rng_unit(void) {
    return ((float)rng_next() + 0.5f) / 4294967296.0f;
}

static float rng_gauss(void) {
    float u1 = ((float)rng_next() + 1.0f) / 4294967296.0f;
    float u2 = ((float)rng_next() + 1.0f) / 4294967296.0f;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

/*
 * Displayed noise floor for this file's gaussian IQ noise on a 1024-point
 * Hann spectrum: 40th percentile of the bins, matching SpectrumWidget.
 * sum(w^2)/N for a Hann window is 0.375, so the mean bin power is 0.75*sigma^2.
 */
static float noise_floor_db(float sigma) {
    return 20.0f * log10f(sigma) - 4.17f;
}

/* On-bin Hann peak, dB, for a carrier of amplitude REF * 10^(atten/20). */
static float tone_power_db(float atten_db) {
    return atten_db + 10.10f;
}

/*
 * Barely detectable: about 18 dB above the displayed noise floor. The spectrum
 * decoder treats a bin as key-down at 16 dB over that floor, and the waterfall
 * leaves black at 8 dB, so this is a dim trace that still copies.
 */
static float barely_atten_db(float sigma) {
    const float snr_db = 18.0f;
    return (noise_floor_db(sigma) + snr_db) - (tone_power_db(0.0f) - 0.0f);
}

static float level_raw(const level_curve_t *curve, float t_s) {
    float raw = 0.0f;
    int i;

    for (i = 0; i < TRAIN_LEVEL_PARTS; ++i) {
        raw += curve->weight[i] *
               sinf(2.0f * (float)M_PI * curve->freq_hz[i] * t_s + curve->phase[i]);
    }
    return raw;
}

/* dB relative to full strength. Clamped to [barely, full]. */
static float level_db(const level_curve_t *curve, float t_s) {
    float raw = level_raw(curve, t_s);
    float span = curve->raw_max - curve->raw_min;
    float unit;
    float db;

    if (span < 1.0e-6f) {
        return curve->full_db;
    }
    unit = (raw - curve->raw_min) / span;
    if (unit < 0.0f) {
        unit = 0.0f;
    } else if (unit > 1.0f) {
        unit = 1.0f;
    }
    db = curve->barely_db + unit * (curve->full_db - curve->barely_db);
    return db;
}

static int build_level_curve(level_curve_t *curve, float sigma) {
    float weight_sum = 0.0f;
    int i;
    int step;
    int steps = TRAIN_SECONDS * 200;

    curve->barely_db = barely_atten_db(sigma);
    curve->full_db = TRAIN_FULL_DB;
    if (!(curve->barely_db < curve->full_db)) {
        fprintf(stderr, "barely-detectable level is not below full strength\n");
        return -1;
    }
    for (i = 0; i < TRAIN_LEVEL_PARTS; ++i) {
        curve->freq_hz[i] = 0.04f + rng_unit() * 0.26f;
        curve->phase[i] = rng_unit() * 2.0f * (float)M_PI;
        curve->weight[i] = 0.35f + rng_unit();
        weight_sum += curve->weight[i];
    }
    for (i = 0; i < TRAIN_LEVEL_PARTS; ++i) {
        curve->weight[i] /= weight_sum;
    }

    curve->raw_min = 1.0e9f;
    curve->raw_max = -1.0e9f;
    for (step = 0; step <= steps; ++step) {
        float raw = level_raw(curve, (float)step / 200.0f);
        if (raw < curve->raw_min) {
            curve->raw_min = raw;
        }
        if (raw > curve->raw_max) {
            curve->raw_max = raw;
        }
    }
    if (!(curve->raw_max > curve->raw_min)) {
        fprintf(stderr, "level curve has no swing\n");
        return -1;
    }
    return 0;
}

/* True when some keyed mark is near full strength and some mark is near the
 * noise. A peak that lands only in a space does not count. */
static int curve_covers_marks(const level_curve_t *curve, const keyed_el_t *els, int n) {
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    int i;

    if (n <= 0) {
        return 0;
    }
    for (i = 0; i < n; ++i) {
        float t_s = ((float)els[i].start + 0.5f * (float)els[i].len) /
                    (float)TRAIN_SAMPLE_RATE;
        float db = level_db(curve, t_s);
        if (db < lo) {
            lo = db;
        }
        if (db > hi) {
            hi = db;
        }
    }
    return lo <= curve->barely_db + 3.0f && hi >= curve->full_db - 3.0f;
}

static int validate_spacing(const train_sig_t *sigs, int n, const char *id) {
    float sorted[TRAIN_MAX_SIGS];
    float min_gap = 1.0e9f;
    float band = (float)TRAIN_SAMPLE_RATE / 2.0f - 500.0f;
    int i;
    int j;

    if (n <= 0) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        sorted[i] = sigs[i].offset_hz;
        if (fabsf(sigs[i].offset_hz) > band) {
            fprintf(stderr, "set %s signal %d at %+.0f Hz is outside the 48 kHz band\n",
                    id, i + 1, sigs[i].offset_hz);
            return -1;
        }
    }
    for (i = 0; i < n; ++i) {
        for (j = i + 1; j < n; ++j) {
            if (sorted[j] < sorted[i]) {
                float tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    for (i = 1; i < n; ++i) {
        float gap = sorted[i] - sorted[i - 1];
        if (gap < min_gap) {
            min_gap = gap;
        }
        if (gap < TRAIN_MIN_SEP_HZ) {
            fprintf(stderr,
                    "set %s carriers at %+.0f and %+.0f Hz are %.0f Hz apart; "
                    "minimum is %.0f Hz\n",
                    id, sorted[i - 1], sorted[i], gap, TRAIN_MIN_SEP_HZ);
            return -1;
        }
    }
    if (n == 1) {
        min_gap = band;
    }
    printf("  carriers %.0f Hz apart or more (minimum %.0f Hz)\n",
           min_gap, TRAIN_MIN_SEP_HZ);
    return 0;
}

static const char *morse_pattern(char ch) {
    switch (toupper((unsigned char)ch)) {
    case 'A': return ".-";
    case 'B': return "-...";
    case 'C': return "-.-.";
    case 'D': return "-..";
    case 'E': return ".";
    case 'F': return "..-.";
    case 'G': return "--.";
    case 'H': return "....";
    case 'I': return "..";
    case 'J': return ".---";
    case 'K': return "-.-";
    case 'L': return ".-..";
    case 'M': return "--";
    case 'N': return "-.";
    case 'O': return "---";
    case 'P': return ".--.";
    case 'Q': return "--.-";
    case 'R': return ".-.";
    case 'S': return "...";
    case 'T': return "-";
    case 'U': return "..-";
    case 'V': return "...-";
    case 'W': return ".--";
    case 'X': return "-..-";
    case 'Y': return "-.--";
    case 'Z': return "--..";
    case '0': return "-----";
    case '1': return ".----";
    case '2': return "..---";
    case '3': return "...--";
    case '4': return "....-";
    case '5': return ".....";
    case '6': return "-....";
    case '7': return "--...";
    case '8': return "---..";
    case '9': return "----.";
    case '/': return "-..-.";
    case '?': return "..--..";
    case '.': return ".-.-.-";
    case ',': return "--..--";
    default: return NULL;
    }
}

/* Key-down intervals for text. Inter-element gap is 1 dit, character gap 3, word gap 7. */
static int build_elements(const train_sig_t *sig, keyed_el_t *els, int max_els,
                          int *out_n, int *out_end) {
    int dit = (int)(1.2f / (float)sig->wpm * (float)TRAIN_SAMPLE_RATE + 0.5f);
    int t = (int)(sig->start_s * (float)TRAIN_SAMPLE_RATE + 0.5f);
    int n = 0;
    int prev_char = 0;
    const char *p;

    if (dit < 1) {
        dit = 1;
    }
    if (t < 0) {
        t = 0;
    }

    for (p = sig->text; *p != '\0'; ++p) {
        const char *pat;
        const char *s;

        if (*p == ' ') {
            t += 7 * dit;
            prev_char = 0;
            continue;
        }
        pat = morse_pattern(*p);
        if (!pat) {
            fprintf(stderr, "unsupported character '%c' in \"%s\"\n", *p, sig->text);
            return -1;
        }
        if (prev_char) {
            t += 3 * dit;
        }
        prev_char = 1;
        for (s = pat; *s != '\0'; ++s) {
            int units = (*s == '-') ? 3 : 1;
            if (n >= max_els) {
                fprintf(stderr, "too many Morse elements in \"%s\"\n", sig->text);
                return -1;
            }
            els[n].start = t;
            els[n].len = units * dit;
            t += els[n].len;
            n++;
            if (s[1] != '\0') {
                t += dit;
            }
        }
    }

    *out_n = n;
    *out_end = t;
    return 0;
}

/* Extra English to keep a tone keyed after its opening phrase. */
static const char *k_sentences[] = {
    "THE CAT IS NEAR",
    "PLEASE SEND A CLEAR COPY",
    "THE QUICK BROWN FOX JUMPS OVER",
    "A WARM SUN DAY",
    "THE SMALL FISH",
    "GOOD DAY TO YOU",
    "THE RIVER IS NEAR",
    "SOFT RAIN",
    "THE NIGHT WIND IS COLD",
    "A BIG RED FOX",
    "HEAR THE LOUD BIRD",
    "THE LAZY FOX AND THE HEN",
    "WATER IS COLD",
    "THE TRAIN IS NEAR",
    "PLEASE READ THE FIRST WORD",
    "THE HOUSE IS WARM",
    "A GREEN FIELD OF SHEEP",
    "THE BOAT IS ON THE LAKE",
    "MANY FISH IN THE RIVER",
    "SEND THIS TRUE WORD AGAIN",
    "CLOSE THE PATH",
    "THE NORTH STAR IS FAR",
    "A FAST RED FOX",
    "THE HORSE IS HOME",
    "RED HEN AND THE DOG",
    "BLUE BIRD IN THE TREE",
    "DARK NIGHT ON THE HILL",
    "GOOD LIGHT ON THE PATH",
    "THE OLD BARN IS NEAR",
    "COPY THIS GOOD WORD",
    "THE SHEEP AND THE GOAT",
    "LEFT OF THE GREEN FIELD",
    "YOU READ THIS SLOW WORD",
    "OPEN THE FAR PATH",
    "A FULL TRUE COPY",
    "THE SUN IS WARM",
    "SEND THE CLEAR WORD",
    "A SOFT WIND",
    "THE DOG IS BIG",
    "FAR HILL AND THE TREE",
};

static int phrase_in_text(const char *buf, const char *phrase) {
    size_t n = strlen(phrase);
    const char *p = buf;

    while ((p = strstr(p, phrase)) != NULL) {
        int before_ok = (p == buf || p[-1] == ' ');
        int after_ok = (p[n] == '\0' || p[n] == ' ');
        if (before_ok && after_ok) {
            return 1;
        }
        p += n;
    }
    return 0;
}

/* Sample time when the last element of text ends. -1 if it will not fit. */
static int text_end_sample(const train_sig_t *sig, const char *text, int limit_sample) {
    train_sig_t tmp;
    keyed_el_t els[TRAIN_MAX_ELS];
    int n = 0;
    int end = 0;

    tmp = *sig;
    tmp.text = text;
    tmp.start_s = TRAIN_LEAD_S;
    if (build_elements(&tmp, els, TRAIN_MAX_ELS, &n, &end) != 0) {
        return -1;
    }
    if (end > limit_sample || n > TRAIN_MAX_ELS) {
        return -1;
    }
    return end;
}

/*
 * Opening phrase, then more dictionary sentences and words, stopping at the
 * last element that still ends inside the file. Text stays within 127 chars
 * so the cw_train line is read in full.
 */
static int compose_full_text(const train_sig_t *sig, int salt, char *buf, int cap,
                             int limit_sample) {
    int nsent = (int)(sizeof(k_sentences) / sizeof(k_sentences[0]));
    int nwords = (int)(sizeof(k_words) / sizeof(k_words[0]));
    int i;

    if (cap < TRAIN_TEXT_MAX + 1) {
        return -1;
    }
    if ((int)strlen(sig->text) > TRAIN_TEXT_MAX) {
        fprintf(stderr, "opening phrase longer than %d characters\n", TRAIN_TEXT_MAX);
        return -1;
    }
    if (text_end_sample(sig, sig->text, limit_sample) < 0) {
        fprintf(stderr, "opening phrase \"%s\" does not fit in the file\n", sig->text);
        return -1;
    }
    snprintf(buf, (size_t)cap, "%s", sig->text);

    for (i = 0; i < nsent; ++i) {
        const char *sent = k_sentences[(salt + i) % nsent];
        char trial[TRAIN_TEXT_MAX + 1];
        int n;

        if (phrase_in_text(buf, sent)) {
            continue;
        }
        n = snprintf(trial, sizeof(trial), "%s %s", buf, sent);
        if (n < 0 || n > TRAIN_TEXT_MAX) {
            continue;
        }
        if (text_end_sample(sig, trial, limit_sample) < 0) {
            continue;
        }
        snprintf(buf, (size_t)cap, "%s", trial);
    }

    for (;;) {
        char best[TRAIN_TEXT_MAX + 1];
        int best_end = -1;
        int w;

        best[0] = '\0';
        for (w = 0; w < nwords; ++w) {
            char trial[TRAIN_TEXT_MAX + 1];
            int n;
            int end;

            n = snprintf(trial, sizeof(trial), "%s %s", buf, k_words[w]);
            if (n < 0 || n > TRAIN_TEXT_MAX) {
                continue;
            }
            end = text_end_sample(sig, trial, limit_sample);
            if (end > best_end) {
                best_end = end;
                snprintf(best, sizeof(best), "%s", trial);
            }
        }
        if (best_end < 0) {
            break;
        }
        snprintf(buf, (size_t)cap, "%s", best);
    }
    return 0;
}

static int keyed_at(const keyed_el_t *els, int n, int *cursor, int t, int *pos, int *len) {
    while (*cursor < n && els[*cursor].start + els[*cursor].len <= t) {
        (*cursor)++;
    }
    if (*cursor < n && t >= els[*cursor].start &&
        t < els[*cursor].start + els[*cursor].len) {
        *pos = t - els[*cursor].start;
        *len = els[*cursor].len;
        return 1;
    }
    return 0;
}

static float edge_env(int pos, int len) {
    int ramp = TRAIN_RAMP_SAMPLES;
    float x;

    if (len <= 1) {
        return 1.0f;
    }
    if (ramp * 2 > len) {
        ramp = len / 2;
    }
    if (ramp < 1) {
        return 1.0f;
    }
    if (pos < ramp) {
        x = (float)pos / (float)ramp;
        return 0.5f - 0.5f * cosf((float)M_PI * x);
    }
    if (pos >= len - ramp) {
        x = (float)(len - 1 - pos) / (float)ramp;
        return 0.5f - 0.5f * cosf((float)M_PI * x);
    }
    return 1.0f;
}

static int write_preamble(FILE *fp, const train_sig_t *sigs, int nsigs) {
    int i;

    if (fprintf(fp, "protocol:TRAIN,1;") < 0 ||
        fprintf(fp, "device:cw-training;") < 0 ||
        fprintf(fp, "dds:0,%lld;", TRAIN_CENTER_HZ) < 0 ||
        fprintf(fp, "vfo:0,0,%lld;", TRAIN_CENTER_HZ) < 0) {
        return -1;
    }
    for (i = 0; i < nsigs; ++i) {
        const train_sig_t *sig = &sigs[i];
        if (strchr(sig->text, ',') || strchr(sig->text, ';')) {
            fprintf(stderr, "transcript must not contain comma or semicolon: %s\n", sig->text);
            return -1;
        }
        if (fprintf(fp, "cw_train:%d,%.1f,%.1f,%.1f,%d,%.3f,%s;",
                    i + 1, sig->offset_hz, sig->atten_db, sig->fade_db,
                    sig->wpm, sig->start_s, sig->text) < 0) {
            return -1;
        }
    }
    return 0;
}

static int write_transcript(const char *path, const train_set_t *set,
                             const train_sig_t *sigs, const float *end_s,
                             FILE *index, FILE *labels) {
    FILE *fp = fopen(path, "w");
    int i;

    if (!fp) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(fp, "# CW skimmer training set %s\n", set->id);
    fprintf(fp, "# stream=set_%s.tcistream\n", set->id);
    fprintf(fp, "# center_hz=%lld sample_rate=%d duration_s=%d\n",
            TRAIN_CENTER_HZ, TRAIN_SAMPLE_RATE, TRAIN_SECONDS);
    fprintf(fp, "# noise_sigma=%.5f  reference_amplitude=%.2f (0 dB, full strength)\n",
            set->noise_sigma, TRAIN_REF_AMPLITUDE);
    fprintf(fp, "# English dictionary words only. Speed is 5 to 30 WPM.\n");
    fprintf(fp, "# Carriers are at least %.0f Hz apart.\n", TRAIN_MIN_SEP_HZ);
    fprintf(fp, "# Each tone's level moves at random from barely detectable "
            "(%.1f dB) to full strength (%.1f dB).\n",
            barely_atten_db(set->noise_sigma), TRAIN_FULL_DB);
    fprintf(fp, "# atten_db is the midpoint of that swing; fade_db is half of it.\n");
    fprintf(fp, "# Each tone starts at %.2fs and is keyed until the file is full.\n",
            TRAIN_LEAD_S);
    fprintf(fp, "# end_s is the sample time when the last Morse element ends.\n");
    fprintf(fp, "# text is exactly the characters that were keyed.\n");
    fprintf(fp, "# index offset_hz atten_db fade_db wpm start_s end_s text\n");
    if (index) {
        fprintf(index, "set %s  center %lld Hz  %d s  noise %.5f\n",
                set->id, TRAIN_CENTER_HZ, TRAIN_SECONDS, set->noise_sigma);
    }
    for (i = 0; i < set->nsigs; ++i) {
        const train_sig_t *sig = &sigs[i];
        fprintf(fp, "%d\t%+.1f\t%.1f\t%.1f\t%d\t%.3f\t%.3f\t%s\n",
                i + 1, sig->offset_hz, sig->atten_db, sig->fade_db,
                sig->wpm, sig->start_s, end_s[i], sig->text);
        printf("  %d  %+.0f Hz  %d WPM  %.2f-%.2fs  %s\n",
               i + 1, sig->offset_hz, sig->wpm, sig->start_s, end_s[i], sig->text);
        if (index) {
            fprintf(index, "  %d\t%+.1f Hz\t%d WPM\t%.3f-%.3fs\t%s\n",
                    i + 1, sig->offset_hz, sig->wpm, sig->start_s, end_s[i], sig->text);
        }
        if (labels) {
            fprintf(labels, "%s\t%d\t%.1f\t%.1f\t%.1f\t%d\t%.3f\t%.3f\t%s\n",
                    set->id, i + 1, sig->offset_hz, sig->atten_db, sig->fade_db,
                    sig->wpm, sig->start_s, end_s[i], sig->text);
        }
    }
    if (index) {
        fprintf(index, "\n");
    }
    fclose(fp);
    return 0;
}

static int write_frame(FILE *fp, const float *interleaved, int pairs) {
    tci_stream_header_t header;

    memset(&header, 0, sizeof(header));
    header.receiver = 0;
    header.sample_rate = TRAIN_SAMPLE_RATE;
    header.format = TCI_AUDIO_FORMAT_FLOAT32;
    header.codec = 0;
    header.crc = 0;
    header.length = (uint32_t)(pairs * TCI_AUDIO_CHANNELS);
    header.type = TCI_STREAM_IQ;
    header.channels = TCI_AUDIO_CHANNELS;

    if (fwrite(&header, 1, sizeof(header), fp) != sizeof(header)) {
        return -1;
    }
    if (fwrite(interleaved, sizeof(float), (size_t)pairs * TCI_AUDIO_CHANNELS, fp) !=
        (size_t)pairs * TCI_AUDIO_CHANNELS) {
        return -1;
    }
    return 0;
}

static int verify_stream(const char *path, int expect_signals) {
    FILE *fp = fopen(path, "rb");
    char token[256];
    int n = 0;
    int trains = 0;
    int saw_dds = 0;
    int c;
    tci_stream_header_t header;

    if (!fp) {
        fprintf(stderr, "cannot reopen %s\n", path);
        return -1;
    }
    while ((c = fgetc(fp)) != EOF) {
        if (n == 0 && c < 32) {
            ungetc(c, fp);
            break;
        }
        if (c == ';') {
            token[n] = '\0';
            if (strncmp(token, "cw_train:", 9) == 0) {
                trains++;
            }
            if (strncmp(token, "dds:", 4) == 0) {
                saw_dds = 1;
            }
            n = 0;
        } else if (n < (int)sizeof(token) - 1) {
            token[n++] = (char)c;
        }
    }
    if (fread(&header, 1, sizeof(header), fp) != sizeof(header)) {
        fprintf(stderr, "%s: missing IQ frame\n", path);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (!saw_dds || trains != expect_signals ||
        header.sample_rate != TRAIN_SAMPLE_RATE ||
        header.format != TCI_AUDIO_FORMAT_FLOAT32 ||
        header.type != TCI_STREAM_IQ ||
        header.channels != TCI_AUDIO_CHANNELS ||
        header.length != (uint32_t)(TRAIN_FRAME_PAIRS * TCI_AUDIO_CHANNELS)) {
        fprintf(stderr, "%s: frame check failed (trains %d header sr %u fmt %u type %u len %u)\n",
                path, trains, header.sample_rate, header.format, header.type, header.length);
        return -1;
    }
    return 0;
}

static int write_set(const char *outdir, const train_set_t *set,
                     FILE *index, FILE *labels) {
    const int num_samples = TRAIN_SAMPLE_RATE * TRAIN_SECONDS;
    const int frames = num_samples / TRAIN_FRAME_PAIRS;
    char stream_path[512];
    char text_path[512];
    keyed_el_t *els[TRAIN_MAX_SIGS];
    int nels[TRAIN_MAX_SIGS];
    int cursor[TRAIN_MAX_SIGS];
    double phase[TRAIN_MAX_SIGS];
    double omega[TRAIN_MAX_SIGS];
    float end_s[TRAIN_MAX_SIGS];
    char full_text[TRAIN_MAX_SIGS][TRAIN_TEXT_MAX + 1];
    train_sig_t local_sigs[TRAIN_MAX_SIGS];
    level_curve_t curves[TRAIN_MAX_SIGS];
    const int limit_sample = num_samples - TRAIN_SAMPLE_RATE / 2;
    int salt_base;
    FILE *fp;
    int i;
    int frame;
    int rc = 0;

    if ((TRAIN_SAMPLE_RATE * TRAIN_SECONDS) % TRAIN_FRAME_PAIRS != 0) {
        fprintf(stderr, "duration is not a whole number of IQ frames\n");
        return -1;
    }
    if (set->nsigs <= 0 || set->nsigs > TRAIN_MAX_SIGS) {
        fprintf(stderr, "set %s has %d signals\n", set->id, set->nsigs);
        return -1;
    }

    salt_base = (set->id[0] - '0') * 10 + (set->id[1] - '0');
    g_rng = 0xC0FFEEu ^ (uint32_t)(set->id[0] * 131u + set->id[1] * 17u);

    memset(els, 0, sizeof(els));
    memset(local_sigs, 0, sizeof(local_sigs));
    for (i = 0; i < set->nsigs; ++i) {
        int end_sample = 0;
        train_sig_t opening;

        memset(&opening, 0, sizeof(opening));
        opening.offset_hz = set->tones[i].offset_hz;
        opening.wpm = set->tones[i].wpm;
        opening.start_s = TRAIN_LEAD_S;
        opening.text = set->tones[i].opening;
        local_sigs[i] = opening;
        if (compose_full_text(&opening, salt_base * 5 + i * 3,
                              full_text[i], (int)sizeof(full_text[i]),
                              limit_sample) != 0) {
            rc = -1;
            break;
        }
        local_sigs[i].text = full_text[i];
        if (validate_signal(&local_sigs[i]) != 0) {
            rc = -1;
            break;
        }
        els[i] = calloc((size_t)TRAIN_MAX_ELS, sizeof(keyed_el_t));
        if (!els[i] ||
            build_elements(&local_sigs[i], els[i], TRAIN_MAX_ELS, &nels[i], &end_sample) != 0) {
            rc = -1;
            break;
        }
        {
            int attempt;
            int covered = 0;

            for (attempt = 0; attempt < 40; ++attempt) {
                if (build_level_curve(&curves[i], set->noise_sigma) != 0) {
                    rc = -1;
                    break;
                }
                if (curve_covers_marks(&curves[i], els[i], nels[i])) {
                    covered = 1;
                    break;
                }
            }
            if (rc != 0) {
                break;
            }
            if (!covered) {
                fprintf(stderr,
                        "set %s signal %d level never reaches both ends while keyed\n",
                        set->id, i + 1);
                rc = -1;
                break;
            }
        }
        local_sigs[i].atten_db = 0.5f * (curves[i].barely_db + curves[i].full_db);
        local_sigs[i].fade_db = 0.5f * (curves[i].full_db - curves[i].barely_db);
        if (end_sample > limit_sample) {
            fprintf(stderr, "set %s signal %d \"%s\" runs to %.2fs, past the 40s file\n",
                    set->id, i + 1, local_sigs[i].text,
                    (double)end_sample / (double)TRAIN_SAMPLE_RATE);
            rc = -1;
            break;
        }
        cursor[i] = 0;
        phase[i] = 0.0;
        end_s[i] = (float)end_sample / (float)TRAIN_SAMPLE_RATE;
        omega[i] = 2.0 * M_PI * (double)local_sigs[i].offset_hz / (double)TRAIN_SAMPLE_RATE;
    }
    if (rc != 0) {
        for (i = 0; i < set->nsigs; ++i) {
            free(els[i]);
        }
        return -1;
    }

    snprintf(stream_path, sizeof(stream_path), "%s/set_%s.tcistream", outdir, set->id);
    snprintf(text_path, sizeof(text_path), "%s/set_%s.txt", outdir, set->id);
    printf("set %s  (%d signals, noise sigma %.5f, level %.1f..%.1f dB)\n",
           set->id, set->nsigs, set->noise_sigma,
           curves[0].barely_db, curves[0].full_db);
    if (validate_spacing(local_sigs, set->nsigs, set->id) != 0) {
        for (i = 0; i < set->nsigs; ++i) {
            free(els[i]);
        }
        return -1;
    }
    if (write_transcript(text_path, set, local_sigs, end_s, index, labels) != 0) {
        for (i = 0; i < set->nsigs; ++i) {
            free(els[i]);
        }
        return -1;
    }

    fp = fopen(stream_path, "wb");
    if (!fp) {
        fprintf(stderr, "cannot write %s: %s\n", stream_path, strerror(errno));
        for (i = 0; i < set->nsigs; ++i) {
            free(els[i]);
        }
        return -1;
    }
    if (write_preamble(fp, local_sigs, set->nsigs) != 0) {
        fprintf(stderr, "preamble write failed for %s\n", stream_path);
        fclose(fp);
        for (i = 0; i < set->nsigs; ++i) {
            free(els[i]);
        }
        return -1;
    }

    g_rng = 0xC0FFEEu ^ (uint32_t)(set->id[0] * 131u + set->id[1] * 17u);

    for (frame = 0; frame < frames; ++frame) {
        float interleaved[TRAIN_FRAME_PAIRS * 2];
        int s;

        for (s = 0; s < TRAIN_FRAME_PAIRS; ++s) {
            int n = frame * TRAIN_FRAME_PAIRS + s;
            float t_s = (float)n / (float)TRAIN_SAMPLE_RATE;
            float i_acc = set->noise_sigma * rng_gauss();
            float q_acc = set->noise_sigma * rng_gauss();
            int k;

            for (k = 0; k < set->nsigs; ++k) {
                int pos = 0;
                int len = 0;
                float amp;
                float env = 0.0f;

                if (keyed_at(els[k], nels[k], &cursor[k], n, &pos, &len)) {
                    amp = TRAIN_REF_AMPLITUDE *
                          powf(10.0f, level_db(&curves[k], t_s) / 20.0f);
                    env = edge_env(pos, len);
                    i_acc += amp * env * (float)cos(phase[k]);
                    q_acc += amp * env * (float)sin(phase[k]);
                }
                phase[k] += omega[k];
                if (phase[k] > 2.0 * M_PI) {
                    phase[k] -= 2.0 * M_PI;
                } else if (phase[k] < -2.0 * M_PI) {
                    phase[k] += 2.0 * M_PI;
                }
            }
            interleaved[s * 2] = i_acc;
            interleaved[s * 2 + 1] = q_acc;
        }
        if (write_frame(fp, interleaved, TRAIN_FRAME_PAIRS) != 0) {
            fprintf(stderr, "frame write failed for %s\n", stream_path);
            rc = -1;
            break;
        }
    }

    if (fclose(fp) != 0) {
        rc = -1;
    }
    for (i = 0; i < set->nsigs; ++i) {
        free(els[i]);
    }
    if (rc != 0) {
        return -1;
    }
    if (verify_stream(stream_path, set->nsigs) != 0) {
        return -1;
    }
    printf("  wrote %s and %s\n", stream_path, text_path);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *outdir = "training";
    char index_path[512];
    char label_path[512];
    FILE *index;
    FILE *labels;
    size_t i;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [output-dir]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        outdir = argv[1];
    }
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s: %s\n", outdir, strerror(errno));
        return 1;
    }

    snprintf(index_path, sizeof(index_path), "%s/transcripts.txt", outdir);
    index = fopen(index_path, "w");
    if (!index) {
        fprintf(stderr, "cannot write %s: %s\n", index_path, strerror(errno));
        return 1;
    }
    fprintf(index, "# Expected CW decode for each training stream.\n");
    fprintf(index, "# English dictionary words only, keyed at 5 to 30 WPM for the whole file.\n");
    fprintf(index, "# Carriers in a file are at least %.0f Hz apart.\n", TRAIN_MIN_SEP_HZ);
    fprintf(index, "# Each tone's level moves at random from barely detectable to full strength.\n");
    fprintf(index, "# The text is exactly what was sent. end time is the last element.\n");
    fprintf(index, "# center %lld Hz, %d Hz sample rate, %d seconds, full 48 kHz band.\n\n",
            TRAIN_CENTER_HZ, TRAIN_SAMPLE_RATE, TRAIN_SECONDS);

    snprintf(label_path, sizeof(label_path), "%s/decoder_labels.tsv", outdir);
    labels = fopen(label_path, "w");
    if (!labels) {
        fprintf(stderr, "cannot write %s: %s\n", label_path, strerror(errno));
        fclose(index);
        return 1;
    }
    fprintf(labels, "set\tindex\toffset_hz\tatten_db\tfade_db\twpm\tstart_s\tend_s\ttext\n");

    for (i = 0; i < sizeof(k_sets) / sizeof(k_sets[0]); ++i) {
        if (write_set(outdir, &k_sets[i], index, labels) != 0) {
            fclose(labels);
            fclose(index);
            return 1;
        }
    }
    fclose(labels);
    fclose(index);
    printf("Wrote %zu training sets in %s\n", i, outdir);
    printf("Transcript index: %s\n", index_path);
    printf("Decoder labels: %s\n", label_path);
    return 0;
}
