/*
 * Morse front end and character decoder transcribed from the unpacked
 * CW Skimmer 2.1 image (child .text at 0x401000, .data at 0x62D000).
 *
 * Addresses refer to that image:
 *   Blackman-Harris window     0x5238e0
 *   sinc                       0x5239b8
 *   windowed-sinc builder      0x523a14
 *   TMainFilter                0x533ef8 / 0x533fac  (512 taps, BW 20..700)
 *   TDspProcessor.SetUp        0x616b8c  (93.75 Hz frame grid, pitch negated)
 *   Gaussian / ProbOfElemLen   0x53c198 / 0x53c1f8
 *   ProbOfLen                  0x53c398
 *   Classify one length        0x53c458
 *   label an element array     0x53c560
 *   WPM                        0x53c104   K = 112.5 = 1.2 * 93.75
 *   hazard table               0x53d988   (BuildTransitionTable)
 *   per-frame HMM step         0x53daf0 / 0x53db94
 *   pattern builder + table    0x53f120 / 0x53edec   table at [0x6342E0]
 *
 * The extractor in the binary is a Viterbi over mark durations 1..20 and
 * space durations 1..30, with an overflow counter past those caps. The
 * caps are an implementation bound (a dah below ~17 WPM already overflows
 * 20 frames). This file runs the same hazard recurrence out to 48 / 96
 * frames so ordinary slow CW still has a bin, using ProbOfLen as the mass
 * of each integer frame. The 8-iteration mixture refit at 0x53c790 (tables
 * 0x630134 and the runtime block at 0x6dc914) is not line-copied; speed
 * tracking below is the median log-length shift of that same routine's
 * sort stage, applied as one global scale on the five log-means.
 */

#include "cwskimmer_decode.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    EL_DIT = 0,
    EL_DAH = 1,
    EL_INTRA = 2,
    EL_CHAR = 3,
    EL_WORD = 4
};

enum {
    MARK_MAX = 48,
    SPACE_MAX = 96,
    NSTATE = MARK_MAX + SPACE_MAX,
    ENV_HIST = 512,
    COMMIT_LAG = 48,
    TEXT_MAX = 8192,
    PAT_MAX = 16,
    FRAME_CAP = 16384
};

/* Priors at 0x6300A8. Marks sum to 1, spaces sum to 1. */
static const double PRIORS[5] = {0.58, 0.42, 0.65, 0.23, 0.12};

/* Log-means at 0x6300BC: dit, dah, intra, char-space, word-space. */
static const double SEED_MEAN[5] = {1.62, 2.70, 1.50, 2.72, 3.56};

/* Variances at 0x630198 (also the diagonal of the 5x5 at 0x6300D0). */
static const double SEED_VAR[5] = {0.0567, 0.0509, 0.071, 0.2, 0.2};

/* 5x5 block copied onto the stats object at +0x57. Symmetric.
   Diagonal matches SEED_VAR. Kept so a later refit can start here. */
static const float SEED_COV[5][5] = {
    {0.0567f, 0.0492f, 0.0454f, 0.0474f, 0.0367f},
    {0.0492f, 0.0509f, 0.0508f, 0.0502f, 0.0385f},
    {0.0454f, 0.0508f, 0.0710f, 0.0602f, 0.0464f},
    {0.0474f, 0.0502f, 0.0602f, 0.2000f, 0.0612f},
    {0.0367f, 0.0385f, 0.0464f, 0.0612f, 0.2000f}
};

#define WPM_K 112.5          /* extended const at 0x53c18c */
#define LN_FLOOR 1e-12       /* SafeLn floor at 0x50b1d0 / 0x50b220 */
#define SLAB_CHAR 0.0001     /* extended const at 0x53c37c */
#define SLAB_WORD 0.1        /* extended const at 0x53c38c */
#define WORD_LN_CAP 300.0    /* immediate 0x43960000 in the type-4 branch */
#define SIGMA_GATE 3.0       /* float 3.0 at 0x53c388 */

struct MorseEntry {
    const char *text;
    const char *pat;
    float weight;
};

/* 64 records, file order. Lookup is exact and stops at the first hit,
   so the 0.1-weight aliases only fire for a non-canonical pattern. */
static const struct MorseEntry MORSE[64] = {
    {"1", ".----", 13},   {"2", "..---", 14},   {"3", "...--", 33},
    {"4", "....-", 43},   {"5", ".....", 41},   {"6", "-....", 8},
    {"7", "--...", 14},   {"8", "---..", 10},   {"9", "----.", 14},
    {"0", "-----", 11},
    {"A", ".-", 127},     {"B", "-...", 62},    {"C", "-.-.", 69},
    {"D", "-..", 84},     {"E", ".", 321},      {"F", "..-.", 55},
    {"G", "--.", 43},     {"H", "....", 68},    {"I", "..", 130},
    {"J", ".---", 8},     {"K", "-.-", 117},    {"L", ".-..", 100},
    {"M", "--", 76},      {"N", "-.", 168},     {"O", "---", 126},
    {"P", ".--.", 57},    {"Q", "--.-", 68},    {"R", ".-.", 95},
    {"S", "...", 159},    {"T", "-", 236},      {"U", "..-", 61},
    {"V", "...-", 23},    {"W", ".--", 95},     {"X", "-..-", 16},
    {"Y", "-.--", 40},    {"Z", "--..", 12},
    {"/", "-..-.", 19},   {".", ".-.-.-", 12},  {",", "--..--", 9},
    {"?", "..--..", 16},  {"=", "-...-", 15},
    {"sk", "...-.-", 7},  {"ar", ".-.-.", 7},   {"as", ".-...", 7},
    {"kn", "-.--.", 7},   {"cq", "-.-.--.-", 7},{"bk", "-...-.-", 7},
    {"dx", "-..-..-", 7},
    {"=", "-....-", 0.1f},{"=", "-.....-", 0.1f},
    {"?", "...--..", 0.1f},{"?", "..--...", 0.1f},
    {"?", "..--.", 0.1f}, {"?", ".--..", 0.1f},
    {"1", ".-----", 0.1f},
    {"2", "..----", 0.1f},{"2", "...---", 0.1f},
    {"3", "....--", 0.1f},{"4", ".....-", 0.1f},
    {"5", "......", 0.1f},{"5", ".......", 0.1f},
    {"6", "-.....", 0.1f},{"7", "--....", 0.1f},
    {"8", "---...", 0.1f}
};

struct CwDecoder {
    double in_rate;
    double pitch;          /* stored after the binary's negation */
    double bandwidth;
    int iq;

    float fir[CW_FIR_TAPS];
    float dl_i[CW_FIR_TAPS];
    float dl_q[CW_FIR_TAPS];
    int dl_pos;

    int in_n;              /* input samples accepted */
    double res_pos;        /* next 6000 Hz output, in input-sample index */
    float res_prev_i;
    float res_prev_q;
    double nco_phase;

    float hop_acc_i;
    float hop_acc_q;
    int hop_n;

    float env_hist[ENV_HIST];
    int env_i;
    int env_n;

    double mean[5];
    double var[5];
    float cov[5][5];

    double ln_end_m[MARK_MAX + 1];
    double ln_cont_m[MARK_MAX + 1];
    double ln_end_s[SPACE_MAX + 1];
    double ln_cont_s[SPACE_MAX + 1];

    float score[NSTATE];
    int16_t *pred;         /* pred[frame * NSTATE + state] */
    int nframes;
    int frame_cap;

    int mark_ln[64];
    int n_mark_ln;
    int adapt_count;

    int elem_len[4096];
    int elem_end[4096];
    int n_elem;
    int folded_len[4096];
    int n_folded;
    char pat[PAT_MAX + 1];
    int np;

    char text[TEXT_MAX];
    int text_len;
    int text_read;
    int finished;
};

static void adapt_speed(CwDecoder *d, int signed_len);

static double safe_ln(double x)
{
    if (!(x > LN_FLOOR))
        x = LN_FLOOR;
    return log(x);
}

/* Normal density in the variance parameterization used at 0x53c198. */
static double gauss_pdf(double x, double var)
{
    if (var < 1e-8)
        var = 1e-8;
    return exp(-x * x / (2.0 * var)) / sqrt(2.0 * M_PI * var);
}

/* p(L | type). L is a positive length in frames. */
static double prob_of_elem_len(const CwDecoder *d, int type, double L)
{
    double lnL, base, tA, tB;

    if (L < 1e-6)
        L = 1e-6;
    type &= 0x7f;
    if (type < 0 || type > 4)
        return 0.0;
    lnL = log(L);
    base = gauss_pdf(lnL - d->mean[type], d->var[type]) / L;

    if (type < 3)
        return PRIORS[type] * base;

    if (type == 3) {
        base *= (PRIORS[3] - SLAB_CHAR);
        /* Thresholds: mean of the neighbour + 3 * sqrt(covariance diag). */
        tA = d->mean[EL_INTRA] + SIGMA_GATE * sqrt(d->var[EL_INTRA]);
        tB = d->mean[EL_CHAR] + SIGMA_GATE * sqrt(d->var[EL_WORD]);
        if (tB > tA && lnL > tA && lnL < tB)
            base += SLAB_CHAR / (tB - tA);
        return base;
    }

    base *= (PRIORS[4] - SLAB_WORD);
    tA = d->mean[EL_CHAR] + SIGMA_GATE * sqrt(d->var[EL_CHAR]);
    tB = WORD_LN_CAP;
    if (tB > tA && lnL > tA && lnL < tB)
        base += SLAB_WORD / (tB - tA);
    return base;
}

/* Positive length: dit or dah. Negative: one of the three spaces. */
static double prob_of_len(const CwDecoder *d, int length)
{
    double a;
    if (length > 0) {
        a = (double)length;
        return prob_of_elem_len(d, EL_DIT, a) + prob_of_elem_len(d, EL_DAH, a);
    }
    a = (double)(length < 0 ? -length : 1);
    return prob_of_elem_len(d, EL_INTRA, a)
         + prob_of_elem_len(d, EL_CHAR, a)
         + prob_of_elem_len(d, EL_WORD, a);
}

static int classify_length(const CwDecoder *d, int length)
{
    double p0, p1, p2, p3, p4, a;

    if (length > 0) {
        a = (double)length;
        p0 = prob_of_elem_len(d, EL_DIT, a);
        p1 = prob_of_elem_len(d, EL_DAH, a);
        return (p0 > p1) ? EL_DIT : EL_DAH;
    }
    a = (double)(length < 0 ? -length : 1);
    p2 = prob_of_elem_len(d, EL_INTRA, a);
    p3 = prob_of_elem_len(d, EL_CHAR, a);
    p4 = prob_of_elem_len(d, EL_WORD, a);
    if (p4 < SLAB_CHAR)
        p4 = SLAB_CHAR;
    if (p2 > p3 && p2 > p4)
        return EL_INTRA;
    return (p3 > p4) ? EL_CHAR : EL_WORD;
}

static const char *lookup_pattern(const char *pat)
{
    int i;
    for (i = 0; i < 64; i++) {
        if (strcmp(MORSE[i].pat, pat) == 0)
            return MORSE[i].text;
    }
    return NULL;
}

/* sinc(n, frac) = sin(2*pi*frac*n) / n, with the n = 0 limit 2*pi*frac.
   frac is cutoff as a fraction of the sample rate (0x5239b8). */
static double cw_sinc(double n, double frac)
{
    double w;
    if (n > -1e-12 && n < 1e-12)
        return 2.0 * M_PI * frac;
    w = 2.0 * M_PI * frac * n;
    return sin(w) / n;
}

/* 4-term Blackman-Harris. Coefficients are the extended constants at
   0x52397c, 0x523970, 0x523994, 0x5239ac. Angles are 2, 4 and 6 pi. */
static double blackman_harris(double t)
{
    const double a0 = 0.35875;
    const double a1 = 0.48829;
    const double a2 = 0.14128;
    const double a3 = 0.01168;
    return a0
         - a1 * cos(2.0 * M_PI * t)
         + a2 * cos(4.0 * M_PI * t)
         - a3 * cos(6.0 * M_PI * t);
}

void cw_analysis_prototype(float *taps, int n, double sample_rate, double cutoff_hz)
{
    double frac, sum, half;
    int i;

    if (n < 1 || sample_rate <= 0.0)
        return;
    if (cutoff_hz < 1.0)
        cutoff_hz = 1.0;
    if (cutoff_hz > sample_rate * 0.45)
        cutoff_hz = sample_rate * 0.45;
    frac = cutoff_hz / sample_rate;
    half = (double)(n / 2);          /* binary uses N/2, not (N-1)/2 */
    sum = 0.0;
    for (i = 0; i < n; i++) {
        double t = (double)i / (double)n;
        double h = cw_sinc((double)i - half, frac) * blackman_harris(t);
        taps[i] = (float)h;
        sum += h;
    }
    if (sum < 1e-12 && sum > -1e-12)
        sum = 1.0;
    for (i = 0; i < n; i++)
        taps[i] = (float)(taps[i] / sum);
}

static void build_hazards(CwDecoder *d)
{
    double acc, mass, h;
    int L;

    acc = 1.0;
    for (L = 1; L <= MARK_MAX; L++) {
        mass = prob_of_len(d, L);
        if (mass < 0.0)
            mass = 0.0;
        h = (acc > 1e-30) ? mass / acc : 1.0;
        if (h < LN_FLOOR)
            h = LN_FLOOR;
        if (h > 1.0 - 1e-9)
            h = 1.0 - 1e-9;
        d->ln_end_m[L] = log(h);
        d->ln_cont_m[L] = log(1.0 - h);
        acc *= (1.0 - h);
        if (acc < 1e-30)
            acc = 1e-30;
    }
    acc = 1.0;
    for (L = 1; L <= SPACE_MAX; L++) {
        mass = prob_of_len(d, -L);
        if (mass < 0.0)
            mass = 0.0;
        h = (acc > 1e-30) ? mass / acc : 1.0;
        if (h < LN_FLOOR)
            h = LN_FLOOR;
        if (h > 1.0 - 1e-9)
            h = 1.0 - 1e-9;
        d->ln_end_s[L] = log(h);
        d->ln_cont_s[L] = log(1.0 - h);
        acc *= (1.0 - h);
        if (acc < 1e-30)
            acc = 1e-30;
    }
}

static int st_mark(int dur) { return dur - 1; }
static int st_space(int dur) { return MARK_MAX + (dur - 1); }
static int st_is_mark(int s) { return s < MARK_MAX; }
static int st_dur(int s)
{
    return st_is_mark(s) ? (s + 1) : (s - MARK_MAX + 1);
}

double cw_decoder_wpm(const CwDecoder *d)
{
    double num, den;
    if (!d)
        return 0.0;
    /* (3*w_dah + w_dit + w_intra) * 112.5 / sum exp(mean)*w */
    num = (3.0 * PRIORS[EL_DAH] + PRIORS[EL_DIT] + PRIORS[EL_INTRA]) * WPM_K;
    den = exp(d->mean[EL_DIT]) * PRIORS[EL_DIT]
        + exp(d->mean[EL_DAH]) * PRIORS[EL_DAH]
        + exp(d->mean[EL_INTRA]) * PRIORS[EL_INTRA];
    if (den < 1e-12)
        return 0.0;
    return num / den;
}

CwDecoder *cw_decoder_create(double sample_rate, double pitch_hz,
                             double bandwidth_hz, int iq)
{
    CwDecoder *d;
    int i;

    if (sample_rate < 100.0)
        return NULL;
    d = (CwDecoder *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->in_rate = sample_rate;
    d->pitch = -pitch_hz;                 /* SetUp negates the CW pitch */
    d->iq = iq ? 1 : 0;
    if (bandwidth_hz < 20.0)
        bandwidth_hz = 20.0;
    if (bandwidth_hz > 700.0)
        bandwidth_hz = 700.0;
    d->bandwidth = bandwidth_hz;

    cw_analysis_prototype(d->fir, CW_FIR_TAPS, CW_INTERNAL_HZ, d->bandwidth);

    for (i = 0; i < 5; i++) {
        d->mean[i] = SEED_MEAN[i];
        d->var[i] = SEED_VAR[i];
    }
    memcpy(d->cov, SEED_COV, sizeof(d->cov));
    build_hazards(d);

    for (i = 0; i < NSTATE; i++)
        d->score[i] = -1.0e30f;

    d->frame_cap = 4096;
    d->pred = (int16_t *)malloc((size_t)d->frame_cap * NSTATE * sizeof(int16_t));
    if (!d->pred) {
        free(d);
        return NULL;
    }
    d->res_pos = 0.0;
    return d;
}

void cw_decoder_destroy(CwDecoder *d)
{
    if (!d)
        return;
    free(d->pred);
    free(d);
}

const char *cw_decoder_text(const CwDecoder *d)
{
    return d ? d->text : "";
}

static int append_text(CwDecoder *d, const char *s)
{
    int n;
    if (!s || !s[0])
        return 0;
    n = (int)strlen(s);
    if (d->text_len + n >= TEXT_MAX)
        n = TEXT_MAX - 1 - d->text_len;
    if (n <= 0)
        return 0;
    memcpy(d->text + d->text_len, s, (size_t)n);
    d->text_len += n;
    d->text[d->text_len] = 0;
    return n;
}

/* Fold newly stable runs into text. Char-space and word-space close a
   character (TMorseDecoder.Process); intra-space does not. A dot/dash
   run is looked up first-match in the 64-entry table. */
static void fold_elements(CwDecoder *d, int n_keep)
{
    int i;
    for (i = d->n_folded; i < n_keep; i++) {
        int type = classify_length(d, d->elem_len[i]);
        d->folded_len[i] = d->elem_len[i];
        if (type == EL_DIT || type == EL_DAH) {
            if (d->np < PAT_MAX)
                d->pat[d->np++] = (type == EL_DIT) ? '.' : '-';
        } else if (type != EL_INTRA) {
            if (d->np > 0) {
                const char *ch;
                d->pat[d->np] = 0;
                ch = lookup_pattern(d->pat);
                if (ch)
                    append_text(d, ch);
                d->np = 0;
            }
            if (type == EL_WORD)
                append_text(d, " ");
        }
        adapt_speed(d, d->elem_len[i]);
    }
    d->n_folded = n_keep;
}

static int ensure_frames(CwDecoder *d, int need)
{
    int16_t *p;
    int cap;
    if (need <= d->frame_cap)
        return 1;
    cap = d->frame_cap;
    while (cap < need) {
        if (cap > FRAME_CAP / 2) {
            cap = FRAME_CAP;
            break;
        }
        cap *= 2;
    }
    if (need > cap)
        return 0;
    p = (int16_t *)realloc(d->pred, (size_t)cap * NSTATE * sizeof(int16_t));
    if (!p)
        return 0;
    d->pred = p;
    d->frame_cap = cap;
    return 1;
}

static void adapt_speed(CwDecoder *d, int signed_len)
{
    int i, n;
    double med, delta;

    if (signed_len <= 0)
        return;
    if (d->n_mark_ln < 64)
        d->mark_ln[d->n_mark_ln++] = signed_len;
    else {
        memmove(d->mark_ln, d->mark_ln + 1, 63 * sizeof(int));
        d->mark_ln[63] = signed_len;
    }
    d->adapt_count++;
    /* The binary refits every 14 stored lengths (counter compared to 0x0E). */
    if (d->adapt_count < 14 || d->n_mark_ln < 7)
        return;
    d->adapt_count = 0;

    /* Insertion sort a scratch copy and take the median, matching the
       quicksort at 0x50b22c followed by a robust location. */
    {
        int a[64];
        n = d->n_mark_ln;
        if (n > 64)
            n = 64;
        memcpy(a, d->mark_ln, (size_t)n * sizeof(int));
        for (i = 1; i < n; i++) {
            int j, v = a[i];
            for (j = i; j > 0 && a[j - 1] > v; j--)
                a[j] = a[j - 1];
            a[j] = v;
        }
        med = log((double)a[n / 2]);
        /* A real CW string has both dits and dahs. A stuck run does not. */
        if (a[n - 1] < a[0] * 2)
            return;
    }
    /* Seed dit is the mixture median (dit prior 0.58 > 0.5). Shift every
       log-mean by the same amount so the dit:dah ratio stays put. */
    delta = med - d->mean[EL_DIT];
    if (delta > 0.8 || delta < -0.8)
        return;
    /* Partial step: the binary's mixture refit is damped by the covariance. */
    delta *= 0.5;
    for (i = 0; i < 5; i++)
        d->mean[i] += delta;
    build_hazards(d);
}

static void viterbi_frame(CwDecoder *d, double p_mark)
{
    float ns[NSTATE];
    int16_t *pred;
    double om, os;
    double best_end_m, best_end_s;
    int best_m_st, best_s_st;
    int dlen, i;

    if (p_mark < 1e-4)
        p_mark = 1e-4;
    if (p_mark > 1.0 - 1e-4)
        p_mark = 1.0 - 1e-4;
    om = safe_ln(p_mark);
    os = safe_ln(1.0 - p_mark);

    if (!ensure_frames(d, d->nframes + 1))
        return;
    pred = d->pred + (size_t)d->nframes * NSTATE;

    for (i = 0; i < NSTATE; i++)
        ns[i] = -1.0e30f;

    if (d->nframes == 0) {
        ns[st_mark(1)] = (float)om;
        ns[st_space(1)] = (float)os;
        pred[st_mark(1)] = -1;
        pred[st_space(1)] = -1;
    } else {
        best_end_m = -1.0e30;
        best_end_s = -1.0e30;
        best_m_st = st_mark(1);
        best_s_st = st_space(1);
        for (dlen = 1; dlen <= MARK_MAX; dlen++) {
            double sc = (double)d->score[st_mark(dlen)] + d->ln_end_m[dlen];
            if (sc > best_end_m) {
                best_end_m = sc;
                best_m_st = st_mark(dlen);
            }
        }
        for (dlen = 1; dlen <= SPACE_MAX; dlen++) {
            double sc = (double)d->score[st_space(dlen)] + d->ln_end_s[dlen];
            if (sc > best_end_s) {
                best_end_s = sc;
                best_s_st = st_space(dlen);
            }
        }

        ns[st_mark(1)] = (float)(best_end_s + om);
        pred[st_mark(1)] = (int16_t)best_s_st;
        ns[st_space(1)] = (float)(best_end_m + os);
        pred[st_space(1)] = (int16_t)best_m_st;

        for (dlen = 2; dlen <= MARK_MAX; dlen++) {
            double sc = (double)d->score[st_mark(dlen - 1)]
                      + d->ln_cont_m[dlen - 1] + om;
            /* Overflow bin: a run already at the cap can stay there. */
            if (dlen == MARK_MAX) {
                double stay = (double)d->score[st_mark(MARK_MAX)]
                            + d->ln_cont_m[MARK_MAX] + om;
                if (stay > sc) {
                    sc = stay;
                    ns[st_mark(MARK_MAX)] = (float)sc;
                    pred[st_mark(MARK_MAX)] = (int16_t)st_mark(MARK_MAX);
                    continue;
                }
            }
            ns[st_mark(dlen)] = (float)sc;
            pred[st_mark(dlen)] = (int16_t)st_mark(dlen - 1);
        }
        for (dlen = 2; dlen <= SPACE_MAX; dlen++) {
            double sc = (double)d->score[st_space(dlen - 1)]
                      + d->ln_cont_s[dlen - 1] + os;
            if (dlen == SPACE_MAX) {
                double stay = (double)d->score[st_space(SPACE_MAX)]
                            + d->ln_cont_s[SPACE_MAX] + os;
                if (stay > sc) {
                    ns[st_space(SPACE_MAX)] = (float)stay;
                    pred[st_space(SPACE_MAX)] = (int16_t)st_space(SPACE_MAX);
                    continue;
                }
            }
            ns[st_space(dlen)] = (float)sc;
            pred[st_space(dlen)] = (int16_t)st_space(dlen - 1);
        }
    }

    memcpy(d->score, ns, sizeof(ns));
    d->nframes++;
}

/* Rebuild elem_* from the best path. Open run at the right edge is kept
   so the caller can drop it. Returns the element count. */
static int backtrace(CwDecoder *d, int drop_open)
{
    int *path;
    int t, best, n, last_mark, run_start;
    float best_sc;

    d->n_elem = 0;
    if (d->nframes <= 0)
        return 0;
    path = (int *)malloc((size_t)d->nframes * sizeof(int));
    if (!path)
        return 0;

    best = 0;
    best_sc = d->score[0];
    for (t = 1; t < NSTATE; t++) {
        if (d->score[t] > best_sc) {
            best_sc = d->score[t];
            best = t;
        }
    }
    path[d->nframes - 1] = best;
    for (t = d->nframes - 1; t > 0; t--) {
        int16_t pr = d->pred[(size_t)t * NSTATE + path[t]];
        if (pr < 0 || pr >= NSTATE)
            pr = path[t];
        path[t - 1] = pr;
    }

    /* A run is a maximal stretch of the same kind whose duration grows.
       Length is the duration on its last frame. */
    n = 0;
    run_start = 0;
    last_mark = st_is_mark(path[0]);
    for (t = 1; t <= d->nframes; t++) {
        int boundary = 0;
        if (t == d->nframes)
            boundary = 1;
        else if (st_is_mark(path[t]) != last_mark || st_dur(path[t]) == 1)
            boundary = 1;
        if (!boundary)
            continue;
        if (!(drop_open && t == d->nframes)) {
            int len = st_dur(path[t - 1]);
            int end = t - 1;
            /* Overflow self-loop reports the cap, not the true length.
               Count the streak of cap-states. */
            if ((last_mark && len == MARK_MAX) || (!last_mark && len == SPACE_MAX)) {
                int k = t - 1;
                int extra = 0;
                while (k >= run_start && st_dur(path[k]) == len
                       && st_is_mark(path[k]) == last_mark) {
                    extra++;
                    k--;
                }
                if (extra > len)
                    len = extra;
            }
            if (!last_mark)
                len = -len;
            if (n < 4096) {
                d->elem_len[n] = len;
                d->elem_end[n] = end;
                n++;
            }
        }
        if (t < d->nframes) {
            run_start = t;
            last_mark = st_is_mark(path[t]);
        }
    }
    free(path);
    d->n_elem = n;
    return n;
}

static void commit_stable(CwDecoder *d, int lag)
{
    int n, i, keep;
    int limit;

    n = backtrace(d, 0);
    if (n <= 0)
        return;
    /* If the frozen prefix moved, leave the text already emitted. */
    for (i = 0; i < d->n_folded && i < n; i++) {
        if (d->elem_len[i] != d->folded_len[i])
            return;
    }
    if (d->n_folded > n)
        return;
    limit = d->nframes - 1 - lag;
    keep = d->n_folded;
    for (i = d->n_folded; i < n; i++) {
        if (d->elem_end[i] <= limit)
            keep = i + 1;
    }
    if (keep > d->n_folded)
        fold_elements(d, keep);
}

static int copy_new(CwDecoder *d, char *out, int out_cap)
{
    int n;
    if (!out || out_cap <= 0)
        return 0;
    n = d->text_len - d->text_read;
    if (n < 0)
        n = 0;
    if (n >= out_cap)
        n = out_cap - 1;
    if (n > 0)
        memcpy(out, d->text + d->text_read, (size_t)n);
    out[n] = 0;
    d->text_read += n;
    return n;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a;
    float y = *(const float *)b;
    return (x > y) - (x < y);
}

static void push_frame_env(CwDecoder *d, float env)
{
    float win[ENV_HIST];
    float recent[48];
    int i, n, nuse, nrec;
    float noise, peak, p;

    d->env_hist[d->env_i] = env;
    d->env_i = (d->env_i + 1) % ENV_HIST;
    if (d->env_n < ENV_HIST)
        d->env_n++;

    /* Noise is a low percentile over ~2 s. Peak is the second-highest
       envelope in the last 0.5 s, so one noisy sample cannot look like
       a keyed dit and a real dit still sets the level. That is the
       sliding-min / sliding-max comparison in the signal extractor. */
    nuse = d->env_n < 200 ? d->env_n : 200;
    for (i = 0; i < nuse; i++) {
        n = d->env_i - 1 - i;
        if (n < 0)
            n += ENV_HIST;
        win[i] = d->env_hist[n];
    }
    qsort(win, (size_t)nuse, sizeof(float), cmp_float);
    noise = win[nuse * 30 / 100];
    nrec = d->env_n < 48 ? d->env_n : 48;
    for (i = 0; i < nrec; i++) {
        n = d->env_i - 1 - i;
        if (n < 0)
            n += ENV_HIST;
        recent[i] = d->env_hist[n];
    }
    qsort(recent, (size_t)nrec, sizeof(float), cmp_float);
    peak = recent[nrec > 1 ? nrec - 2 : 0];
    /* The first quarter-second is silence in the filter delay line. */
    if (nuse < 20 || peak < noise * 2.5f + 1e-5f)
        p = 0.04f;
    else {
        p = (env - noise) / (peak - noise);
        if (p < 0.04f)
            p = 0.04f;
        if (p > 0.96f)
            p = 0.96f;
    }
    viterbi_frame(d, (double)p);
    if (d->nframes > COMMIT_LAG + 8)
        commit_stable(d, COMMIT_LAG);
}

static void consume_baseband(CwDecoder *d, float i, float q)
{
    float yi, yq;
    int k, idx;

    d->dl_i[d->dl_pos] = i;
    d->dl_q[d->dl_pos] = q;
    yi = 0.0f;
    yq = 0.0f;
    idx = d->dl_pos;
    for (k = 0; k < CW_FIR_TAPS; k++) {
        yi += d->fir[k] * d->dl_i[idx];
        yq += d->fir[k] * d->dl_q[idx];
        if (--idx < 0)
            idx = CW_FIR_TAPS - 1;
    }
    d->dl_pos++;
    if (d->dl_pos >= CW_FIR_TAPS)
        d->dl_pos = 0;

    d->hop_acc_i += yi;
    d->hop_acc_q += yq;
    d->hop_n++;
    if (d->hop_n >= 64) {
        float mi = d->hop_acc_i / 64.0f;
        float mq = d->hop_acc_q / 64.0f;
        float env = sqrtf(mi * mi + mq * mq);
        d->hop_acc_i = 0.0f;
        d->hop_acc_q = 0.0f;
        d->hop_n = 0;
        push_frame_env(d, env);
    }
}

/* Linear resample onto the 6000 Hz grid. The binary's octave + polyphase
   resamplers land on this same grid (round(sampleRate/93.75), then the
   bandwidth decimation). Output time res_pos is an input-sample index. */
static void feed_sample(CwDecoder *d, float i, float q)
{
    double step = d->in_rate / CW_INTERNAL_HZ;
    double frac;
    float xi, xq, co, si, yr, yi;

    if (d->in_n == 0) {
        d->res_prev_i = i;
        d->res_prev_q = q;
        d->res_pos = 0.0;
        d->nco_phase = 0.0;
    }

    while (d->res_pos <= (double)d->in_n + 1e-9) {
        if (d->in_n == 0) {
            xi = i;
            xq = q;
        } else {
            frac = d->res_pos - (double)(d->in_n - 1);
            if (frac < 0.0)
                frac = 0.0;
            if (frac > 1.0)
                frac = 1.0;
            xi = (float)((1.0 - frac) * d->res_prev_i + frac * i);
            xq = (float)((1.0 - frac) * d->res_prev_q + frac * q);
        }

        /* NCO at the internal rate. d->pitch is already negated. */
        co = (float)cos(d->nco_phase);
        si = (float)sin(d->nco_phase);
        d->nco_phase += 2.0 * M_PI * d->pitch / CW_INTERNAL_HZ;
        if (d->nco_phase > 8.0 * M_PI || d->nco_phase < -8.0 * M_PI)
            d->nco_phase = fmod(d->nco_phase, 2.0 * M_PI);
        yr = xi * co - xq * si;
        yi = xi * si + xq * co;
        consume_baseband(d, yr, yi);

        d->res_pos += step;
        if (d->nframes >= FRAME_CAP - 1)
            break;
    }
    d->res_prev_i = i;
    d->res_prev_q = q;
    d->in_n++;
}

int cw_decoder_process(CwDecoder *d, const float *samples, int n,
                       char *out, int out_cap)
{
    int i;

    if (!d || !samples || n <= 0)
        return copy_new(d, out, out_cap);
    if (d->iq) {
        int pairs = n / 2;
        for (i = 0; i < pairs; i++)
            feed_sample(d, samples[2 * i], samples[2 * i + 1]);
    } else {
        for (i = 0; i < n; i++)
            feed_sample(d, samples[i], 0.0f);
    }
    return copy_new(d, out, out_cap);
}

int cw_decoder_finish(CwDecoder *d, char *out, int out_cap)
{
    int n, i;
    if (!d)
        return 0;
    /* Close the hop so a trailing partial block still yields a frame. */
    if (d->hop_n > 0) {
        float mi = d->hop_acc_i / (float)d->hop_n;
        float mq = d->hop_acc_q / (float)d->hop_n;
        d->hop_acc_i = 0.0f;
        d->hop_acc_q = 0.0f;
        d->hop_n = 0;
        push_frame_env(d, sqrtf(mi * mi + mq * mq));
    }
    if (d->finished)
        return copy_new(d, out, out_cap);
    n = backtrace(d, 0);
    for (i = 0; i < d->n_folded && i < n; i++) {
        if (d->elem_len[i] != d->folded_len[i]) {
            d->finished = 1;
            return copy_new(d, out, out_cap);
        }
    }
    /* Close a trailing open character (no final space yet). */
    if (n > d->n_folded)
        fold_elements(d, n);
    if (d->np > 0) {
        const char *ch;
        d->pat[d->np] = 0;
        ch = lookup_pattern(d->pat);
        if (ch)
            append_text(d, ch);
        d->np = 0;
    }
    d->finished = 1;
    return copy_new(d, out, out_cap);
}
