#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "deepcw_engine.h"
#include "logger.h"

#include <onnxruntime_c_api.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEEPCW_MAX_TEXT          256
#define DEEPCW_MAX_AUDIO_SEC     20
#define DEEPCW_MIN_INFER_SEC     2
#define DEEPCW_INFER_INTERVAL_MS 1000
#define DEEPCW_SHIFT_CENTER_HZ   800.0f
#define DEEPCW_SHIFT_BW_HZ       50.0f
#define DEEPCW_TONE_RESET_HZ     20.0f

static const OrtApi *g_ort = NULL;
static OrtEnv *g_env = NULL;
static OrtSession *g_session = NULL;
static OrtSessionOptions *g_session_opts = NULL;
static OrtMemoryInfo *g_mem_info = NULL;
static int g_global_ready = 0;

static deepcw_metadata_t g_meta = {
    .sample_rate = 3200,
    .fft_length = 256,
    .hop_length = 48,
    .min_freq_hz = 400.0f,
    .max_freq_hz = 1200.0f,
    .freq_bins = 65,
    .blank_index = 41,
    .num_classes = 42,
    .input_name = "spectrogram",
    .output_name = "log_probs",
    .vocabulary = ",./0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZ "
};

struct deepcw_engine {
    float tone_hz;
    float *audio;
    int audio_cap;
    int audio_len;

    char text[DEEPCW_MAX_TEXT];
    char plain_text[DEEPCW_MAX_TEXT];
    char pending[DEEPCW_MAX_TEXT];
    char prev_text[DEEPCW_MAX_TEXT];
    char prev_plain_text[DEEPCW_MAX_TEXT];

    char new_letter;
    int has_new_letter;
    int dirty;

    long long source_sample_index;
    int samples_since_infer;
    int infer_interval_samples;
    float resample_phase;
};

static float deepcw_bin_resolution(void)
{
    return (float)g_meta.sample_rate / (float)g_meta.fft_length;
}

static int deepcw_freq_to_bin(float freq_hz)
{
    return (int)lroundf(freq_hz / deepcw_bin_resolution());
}

static void deepcw_hann_window(float *window, int n)
{
    int i;
    for (i = 0; i < n; ++i) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)n));
    }
}

static void deepcw_fft_radix2(float *real, float *imag, int n)
{
    int i, j, len;
    int bits = 0;
    int nn = n;

    while (nn > 1) {
        bits++;
        nn >>= 1;
    }

    for (i = 0; i < n; ++i) {
        int rev = 0;
        int x = i;
        for (j = 0; j < bits; ++j) {
            rev = (rev << 1) | (x & 1);
            x >>= 1;
        }
        if (i < rev) {
            float tr = real[i];
            float ti = imag[i];
            real[i] = real[rev];
            imag[i] = imag[rev];
            real[rev] = tr;
            imag[rev] = ti;
        }
    }

    for (len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wlen_r = cosf(ang);
        float wlen_i = sinf(ang);
        for (i = 0; i < n; i += len) {
            float w_r = 1.0f;
            float w_i = 0.0f;
            for (j = 0; j < len / 2; ++j) {
                int u = i + j;
                int v = i + j + len / 2;
                float t_r = w_r * real[v] - w_i * imag[v];
                float t_i = w_r * imag[v] + w_i * real[v];
                float u_r = real[u];
                float u_i = imag[u];
                real[u] = u_r + t_r;
                imag[u] = u_i + t_i;
                real[v] = u_r - t_r;
                imag[v] = u_i - t_i;
                float nw_r = w_r * wlen_r - w_i * wlen_i;
                float nw_i = w_r * wlen_i + w_i * wlen_r;
                w_r = nw_r;
                w_i = nw_i;
            }
        }
    }
}

static void deepcw_normalize_cmvn(float *data, int count)
{
    float mean = 0.0f;
    float var = 0.0f;
    int i;

    if (count <= 0) {
        return;
    }

    for (i = 0; i < count; ++i) {
        mean += data[i];
    }
    mean /= (float)count;
    for (i = 0; i < count; ++i) {
        float d = data[i] - mean;
        var += d * d;
    }
    {
        float std = sqrtf(var / (float)count);
        if (std < 1e-5f) {
            std = 1e-5f;
        }
        for (i = 0; i < count; ++i) {
            data[i] = (data[i] - mean) / std;
        }
    }
}

static int deepcw_build_spectrogram(const float *audio, int audio_len,
                                    float *out_spec, int *out_frames)
{
    const int fft_n = g_meta.fft_length;
    const int hop = g_meta.hop_length;
    const int bins = g_meta.freq_bins;
    int start_bin;
    int stop_bin;
    float window[256];
    float frame_r[256];
    float frame_i[256];
    int frames;
    int fi;
    int pad;

    if (!audio || audio_len < fft_n || !out_spec || !out_frames) {
        return 0;
    }

    start_bin = (int)ceilf(g_meta.min_freq_hz / deepcw_bin_resolution());
    stop_bin = (int)floorf(g_meta.max_freq_hz / deepcw_bin_resolution()) + 1;
    if (stop_bin - start_bin != bins) {
        return 0;
    }

    deepcw_hann_window(window, fft_n);
    frames = 1 + (audio_len - fft_n) / hop;
    if (frames <= 0) {
        return 0;
    }

    pad = fft_n / 2;
    for (fi = 0; fi < frames; ++fi) {
        int start = fi * hop;
        int bi;
        int offset = fi * bins;

        for (bi = 0; bi < fft_n; ++bi) {
            int src = start + bi - pad;
            float sample = 0.0f;
            if (src < 0) {
                sample = audio[-src];
            } else if (src >= audio_len) {
                sample = audio[audio_len - 1 - (src - audio_len)];
            } else {
                sample = audio[src];
            }
            frame_r[bi] = sample * window[bi];
            frame_i[bi] = 0.0f;
        }

        deepcw_fft_radix2(frame_r, frame_i, fft_n);

        for (bi = 0; bi < bins; ++bi) {
            int src_bin = start_bin + bi;
            float mag = hypotf(frame_r[src_bin], frame_i[src_bin]);
            out_spec[offset + bi] = mag;
        }
    }

    deepcw_normalize_cmvn(out_spec, frames * bins);
    *out_frames = frames;
    return 1;
}

static void deepcw_ctc_decode_plain(const int *indices, int count, char *out, int out_max)
{
    int previous = -1;
    int o = 0;
    int i;

    if (!out || out_max <= 0) {
        return;
    }
    out[0] = '\0';

    for (i = 0; i < count; ++i) {
        int idx = indices[i];
        if (idx == g_meta.blank_index) {
            previous = -1;
            continue;
        }
        if (idx == previous) {
            continue;
        }
        previous = idx;
        if (idx >= 0 && idx < (int)strlen(g_meta.vocabulary)) {
            if (o + 1 < out_max) {
                out[o++] = g_meta.vocabulary[idx];
            }
        }
    }
    out[o] = '\0';
}

static void deepcw_ctc_decode_display(const int *indices, int count, char *out, int out_max)
{
    int previous = -1;
    int o = 0;
    int i;

    if (!out || out_max <= 0) {
        return;
    }
    out[0] = '\0';

    for (i = 0; i < count; ++i) {
        int idx = indices[i];
        if (idx == g_meta.blank_index) {
            if (o + 1 < out_max) {
                out[o++] = ' ';
            }
            previous = -1;
            continue;
        }
        if (idx == previous) {
            if (o + 1 < out_max) {
                out[o++] = ' ';
            }
            continue;
        }
        previous = idx;
        if (idx >= 0 && idx < (int)strlen(g_meta.vocabulary)) {
            if (o + 1 < out_max) {
                out[o++] = g_meta.vocabulary[idx];
            }
        }
    }
    out[o] = '\0';
}

static int deepcw_run_inference(deepcw_engine_t *engine, const float *audio, int audio_len)
{
    int max_frames;
    float *spec;
    int frames = 0;
    int64_t input_shape[4];
    OrtValue *input_tensor = NULL;
    OrtValue *output_tensor = NULL;
    const char *input_names[] = { g_meta.input_name };
    const char *output_names[] = { g_meta.output_name };
    const float *log_probs = NULL;
    int64_t out_shape[3];
    size_t out_dims = 0;
    int t, c;
    int *best_path = NULL;
    char decoded[DEEPCW_MAX_TEXT];
    char plain[DEEPCW_MAX_TEXT];
    int status = 0;

    if (!engine || !g_global_ready || !audio || audio_len < g_meta.fft_length) {
        return 0;
    }

    max_frames = 1 + (audio_len - g_meta.fft_length) / g_meta.hop_length;
    spec = calloc((size_t)max_frames * (size_t)g_meta.freq_bins, sizeof(float));
    if (!spec) {
        return 0;
    }

    if (!deepcw_build_spectrogram(audio, audio_len, spec, &frames) ||
        frames <= 0) {
        free(spec);
        return 0;
    }

    input_shape[0] = 1;
    input_shape[1] = 1;
    input_shape[2] = frames;
    input_shape[3] = g_meta.freq_bins;

    if (g_ort->CreateTensorWithDataAsOrtValue(
            g_mem_info, spec, (size_t)frames * (size_t)g_meta.freq_bins * sizeof(float),
            input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor) != NULL) {
        free(spec);
        return 0;
    }

    if (g_ort->Run(g_session, NULL, input_names, (const OrtValue *const *)&input_tensor, 1,
                   output_names, 1, &output_tensor) != NULL) {
        g_ort->ReleaseValue(input_tensor);
        free(spec);
        return 0;
    }

    {
        OrtTensorTypeAndShapeInfo *shape_info = NULL;
        if (g_ort->GetTensorTypeAndShape(output_tensor, &shape_info) != NULL ||
            g_ort->GetDimensionsCount(shape_info, &out_dims) != NULL ||
            out_dims < 3 ||
            g_ort->GetDimensions(shape_info, out_shape, out_dims) != NULL) {
            if (shape_info) {
                g_ort->ReleaseTensorTypeAndShapeInfo(shape_info);
            }
            g_ort->ReleaseValue(input_tensor);
            g_ort->ReleaseValue(output_tensor);
            free(spec);
            return 0;
        }
        g_ort->ReleaseTensorTypeAndShapeInfo(shape_info);
    }

    if (g_ort->GetTensorMutableData(output_tensor, (void **)&log_probs) != NULL) {
        g_ort->ReleaseValue(input_tensor);
        g_ort->ReleaseValue(output_tensor);
        free(spec);
        return 0;
    }

    best_path = calloc((size_t)out_shape[1], sizeof(int));
    if (!best_path) {
        g_ort->ReleaseValue(input_tensor);
        g_ort->ReleaseValue(output_tensor);
        free(spec);
        return 0;
    }

    for (t = 0; t < (int)out_shape[1]; ++t) {
        int best_idx = 0;
        float best_val = log_probs[t * (int)out_shape[2]];
        for (c = 1; c < (int)out_shape[2]; ++c) {
            float val = log_probs[t * (int)out_shape[2] + c];
            if (val > best_val) {
                best_val = val;
                best_idx = c;
            }
        }
        best_path[t] = best_idx;
    }

    deepcw_ctc_decode_display(best_path, (int)out_shape[1], decoded, sizeof(decoded));
    deepcw_ctc_decode_plain(best_path, (int)out_shape[1], plain, sizeof(plain));

    {
        size_t old_len = strlen(engine->plain_text);
        size_t new_len = strlen(plain);
        if (new_len > old_len) {
            engine->new_letter = plain[new_len - 1];
            engine->has_new_letter = 1;
        } else {
            engine->has_new_letter = 0;
        }
        strncpy(engine->pending, decoded, sizeof(engine->pending) - 1);
        engine->pending[sizeof(engine->pending) - 1] = '\0';

        if (strcmp(engine->prev_text, decoded) != 0 ||
            strcmp(engine->prev_plain_text, plain) != 0) {
            strncpy(engine->text, decoded, sizeof(engine->text) - 1);
            engine->text[sizeof(engine->text) - 1] = '\0';
            strncpy(engine->plain_text, plain, sizeof(engine->plain_text) - 1);
            engine->plain_text[sizeof(engine->plain_text) - 1] = '\0';
            strncpy(engine->prev_text, decoded, sizeof(engine->prev_text) - 1);
            engine->prev_text[sizeof(engine->prev_text) - 1] = '\0';
            strncpy(engine->prev_plain_text, plain, sizeof(engine->prev_plain_text) - 1);
            engine->prev_plain_text[sizeof(engine->prev_plain_text) - 1] = '\0';
            engine->dirty = 1;
            status = 1;
        }
    }

    free(best_path);
    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseValue(output_tensor);
    free(spec);
    return status;
}

static int deepcw_append_audio(deepcw_engine_t *engine, float sample)
{
    if (!engine) {
        return 0;
    }
    if (engine->audio_len >= engine->audio_cap) {
        int new_cap = engine->audio_cap > 0 ? engine->audio_cap * 2 : g_meta.sample_rate * 4;
        float *grown = realloc(engine->audio, (size_t)new_cap * sizeof(float));
        if (!grown) {
            return 0;
        }
        engine->audio = grown;
        engine->audio_cap = new_cap;
    }
    engine->audio[engine->audio_len++] = sample;
    return 1;
}

static void deepcw_trim_audio(deepcw_engine_t *engine, int keep_samples)
{
    if (!engine || keep_samples <= 0) {
        return;
    }
    if (engine->audio_len <= keep_samples) {
        return;
    }
    memmove(engine->audio, engine->audio + engine->audio_len - keep_samples,
            (size_t)keep_samples * sizeof(float));
    engine->audio_len = keep_samples;
}

static int deepcw_maybe_infer(deepcw_engine_t *engine)
{
    int min_samples = g_meta.sample_rate * DEEPCW_MIN_INFER_SEC;
    int max_samples = g_meta.sample_rate * DEEPCW_MAX_AUDIO_SEC;

    if (!engine || engine->audio_len < min_samples) {
        return 0;
    }

    if (engine->samples_since_infer < engine->infer_interval_samples) {
        return 0;
    }

    if (engine->audio_len > max_samples) {
        deepcw_trim_audio(engine, max_samples);
    }

    engine->samples_since_infer = 0;
    return deepcw_run_inference(engine, engine->audio, engine->audio_len);
}

int deepcw_engine_global_init(const char *model_path)
{
    if (g_global_ready) {
        return 0;
    }
    if (!model_path || model_path[0] == '\0') {
        LOG_ERROR("DeepCW model path is empty");
        return -1;
    }

    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        LOG_ERROR("Failed to get ONNX Runtime API");
        return -1;
    }

    if (g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "cw-skimmer", &g_env) != NULL) {
        LOG_ERROR("Failed to create ONNX Runtime environment");
        return -1;
    }

    if (g_ort->CreateSessionOptions(&g_session_opts) != NULL) {
        LOG_ERROR("Failed to create ONNX session options");
        return -1;
    }

    if (g_ort->CreateSession(g_env, model_path, g_session_opts, &g_session) != NULL) {
        LOG_ERROR("Failed to load DeepCW model: %s", model_path);
        return -1;
    }

    if (g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_mem_info) != NULL) {
        LOG_ERROR("Failed to create ONNX memory info");
        return -1;
    }

    g_global_ready = 1;
    LOG_INFO("DeepCW model loaded: %s (sr=%d fft=%d)", model_path,
             g_meta.sample_rate, g_meta.fft_length);
    return 0;
}

void deepcw_engine_global_shutdown(void)
{
    if (g_mem_info) {
        g_ort->ReleaseMemoryInfo(g_mem_info);
        g_mem_info = NULL;
    }
    if (g_session) {
        g_ort->ReleaseSession(g_session);
        g_session = NULL;
    }
    if (g_session_opts) {
        g_ort->ReleaseSessionOptions(g_session_opts);
        g_session_opts = NULL;
    }
    if (g_env) {
        g_ort->ReleaseEnv(g_env);
        g_env = NULL;
    }
    g_global_ready = 0;
}

int deepcw_engine_global_ready(void)
{
    return g_global_ready;
}

deepcw_engine_t *deepcw_engine_create(void)
{
    deepcw_engine_t *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        return NULL;
    }
    engine->tone_hz = DEEPCW_SHIFT_CENTER_HZ;
    engine->infer_interval_samples = g_meta.sample_rate;
    return engine;
}

void deepcw_engine_destroy(deepcw_engine_t *engine)
{
    if (!engine) {
        return;
    }
    free(engine->audio);
    free(engine);
}

void deepcw_engine_reset(deepcw_engine_t *engine)
{
    if (!engine) {
        return;
    }
    engine->audio_len = 0;
    engine->source_sample_index = 0;
    engine->samples_since_infer = 0;
    engine->text[0] = '\0';
    engine->plain_text[0] = '\0';
    engine->pending[0] = '\0';
    engine->prev_text[0] = '\0';
    engine->prev_plain_text[0] = '\0';
    engine->new_letter = '\0';
    engine->has_new_letter = 0;
    engine->dirty = 0;
}

void deepcw_engine_set_tone_hz(deepcw_engine_t *engine, float tone_hz)
{
    if (!engine) {
        return;
    }
    if (fabsf(engine->tone_hz - tone_hz) > DEEPCW_TONE_RESET_HZ) {
        engine->tone_hz = tone_hz;
        deepcw_engine_reset(engine);
    } else {
        engine->tone_hz = tone_hz;
    }
}

static int deepcw_feed_iq_internal(deepcw_engine_t *engine,
                                   const float *iq_real,
                                   const float *iq_imag,
                                   int count,
                                   int source_sample_rate,
                                   int allow_infer)
{
    int i;
    int out_count = 0;
    float ratio;
    float omega;

    if (!engine || !iq_real || !iq_imag || count <= 0 || source_sample_rate <= 0) {
        return 0;
    }

    ratio = (float)source_sample_rate / (float)g_meta.sample_rate;
    /* Mix so the tracked tone lands at the model center frequency. */
    omega = 2.0f * (float)M_PI *
            (engine->tone_hz - DEEPCW_SHIFT_CENTER_HZ) / (float)source_sample_rate;

    for (i = 0; i < count; ++i) {
        float phase = omega * (float)engine->source_sample_index;
        float c = cosf(phase);
        float s = sinf(phase);
        float mix_i = iq_real[i] * c + iq_imag[i] * s;
        float sample = mix_i;

        engine->resample_phase += 1.0f;
        while (engine->resample_phase >= ratio) {
            engine->resample_phase -= ratio;
            if (deepcw_append_audio(engine, sample)) {
                out_count++;
                engine->samples_since_infer++;
            }
        }
        engine->source_sample_index++;
    }

    if (!allow_infer) {
        return out_count > 0 ? 1 : 0;
    }
    return deepcw_maybe_infer(engine);
}

int deepcw_engine_feed_iq(deepcw_engine_t *engine,
                          const float *iq_real,
                          const float *iq_imag,
                          int count,
                          int source_sample_rate)
{
    return deepcw_feed_iq_internal(engine, iq_real, iq_imag, count,
                                   source_sample_rate, 1);
}

int deepcw_engine_feed_iq_no_infer(deepcw_engine_t *engine,
                                   const float *iq_real,
                                   const float *iq_imag,
                                   int count,
                                   int source_sample_rate)
{
    return deepcw_feed_iq_internal(engine, iq_real, iq_imag, count,
                                   source_sample_rate, 0);
}

int deepcw_engine_try_infer(deepcw_engine_t *engine)
{
    if (!engine) {
        return -1;
    }
    return deepcw_maybe_infer(engine) ? 1 : 0;
}

int deepcw_engine_audio_len(const deepcw_engine_t *engine)
{
    return engine ? engine->audio_len : 0;
}

int deepcw_engine_samples_since_infer(const deepcw_engine_t *engine)
{
    return engine ? engine->samples_since_infer : 0;
}

int deepcw_engine_infer_interval_samples(const deepcw_engine_t *engine)
{
    return engine ? engine->infer_interval_samples : 0;
}

int deepcw_engine_flush(deepcw_engine_t *engine)
{
    if (!engine || engine->audio_len < g_meta.fft_length) {
        return 0;
    }
    engine->samples_since_infer = 0;
    return deepcw_run_inference(engine, engine->audio, engine->audio_len);
}

const char *deepcw_engine_get_text(const deepcw_engine_t *engine)
{
    return engine ? engine->text : "";
}

const char *deepcw_engine_get_plain_text(const deepcw_engine_t *engine)
{
    return engine ? engine->plain_text : "";
}

const char *deepcw_engine_get_pending(const deepcw_engine_t *engine)
{
    return engine ? engine->pending : "";
}

int deepcw_engine_take_new_letter(deepcw_engine_t *engine, char *out)
{
    if (!engine || !out) {
        return 0;
    }
    if (!engine->has_new_letter) {
        *out = '\0';
        return 0;
    }
    *out = engine->new_letter;
    engine->has_new_letter = 0;
    engine->new_letter = '\0';
    return 1;
}

int deepcw_engine_is_dirty(const deepcw_engine_t *engine)
{
    return engine ? engine->dirty : 0;
}

void deepcw_engine_clear_dirty(deepcw_engine_t *engine)
{
    if (engine) {
        engine->dirty = 0;
    }
}

const deepcw_metadata_t *deepcw_engine_get_metadata(void)
{
    return &g_meta;
}