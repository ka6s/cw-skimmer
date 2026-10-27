/**
 * DeepCW ONNX decoder.
 *
 * The spectrogram, noise normalization, CTC stitcher, and streaming
 * windows follow decode_morse.py in e04/deepcw-engine. Names and
 * constants match that file. The radio front end below the decoder
 * only builds the mono stream that file reads from a WAV.
 */

#include "deepcw_engine.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace {

constexpr double kNoiseLevelMinRelativeToPeak = 1e-3;
constexpr double kNoiseLevelMin = 1e-5;
constexpr double kMaxSegmentSeconds = 12.0;
constexpr double kMinPendingSeconds = 2.0;
constexpr double kStepSeconds = 1.0;
constexpr double kStitchLeftContextSeconds = 4.0;
constexpr double kStitchFreezeGuardSeconds = 2.0;
constexpr double kStitchLeftRampSeconds = 2.0;
constexpr double kStitchRightRampSeconds = 1.0;
constexpr int kStitchDilationFrames = 1;
constexpr int kWordBoundaryBlankHops = 134;
constexpr double kMinViewWeight = 1e-3;
constexpr float kUncoveredLogProb = -30.0f;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Metadata {
    std::vector<char> chars;
    int blank_index;
    int sample_rate;
    int fft_length;
    int hop_length;
    int output_time_stride;
    double min_hz;
    double max_hz;
    int frequency_bins;
    double noise_quantile;
    std::string input_name;
    std::string output_name;
    int num_classes;
};

int python_int(double value)
{
    return static_cast<int>(value);
}

int reflect_index(int k, int length)
{
    if (length <= 1) {
        return 0;
    }
    const int period = 2 * (length - 1);
    int index = k % period;
    if (index < 0) {
        index += period;
    }
    if (index >= length) {
        index = period - index;
    }
    return index;
}

std::string read_file(const char *path, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = std::string("Cannot read ") + path;
        }
        return std::string();
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool extract_number(const std::string &json, const char *key, double *out)
{
    const std::string pattern = std::string("\"") + key + "\"";
    const size_t key_at = json.find(pattern);
    if (key_at == std::string::npos) {
        return false;
    }
    size_t colon = json.find(':', key_at + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    char *end = nullptr;
    const double value = std::strtod(json.c_str() + colon + 1, &end);
    if (end == json.c_str() + colon + 1) {
        return false;
    }
    *out = value;
    return true;
}

bool extract_string(const std::string &json, const char *key, std::string *out)
{
    const std::string pattern = std::string("\"") + key + "\"";
    const size_t key_at = json.find(pattern);
    if (key_at == std::string::npos) {
        return false;
    }
    const size_t colon = json.find(':', key_at + pattern.size());
    const size_t open = json.find('"', colon + 1);
    const size_t close = json.find('"', open + 1);
    if (colon == std::string::npos || open == std::string::npos || close == std::string::npos) {
        return false;
    }
    *out = json.substr(open + 1, close - open - 1);
    return true;
}

bool load_metadata(const char *path, Metadata *meta, std::string *error)
{
    const std::string json = read_file(path, error);
    if (json.empty()) {
        return false;
    }

    const size_t chars_key = json.find("\"chars\"");
    const size_t open = chars_key == std::string::npos ? std::string::npos : json.find('[', chars_key);
    const size_t close = open == std::string::npos ? std::string::npos : json.find(']', open);
    if (open == std::string::npos || close == std::string::npos) {
        if (error) {
            *error = "DeepCW metadata has no chars list";
        }
        return false;
    }
    meta->chars.clear();
    for (size_t i = open + 1; i < close; ++i) {
        if (json[i] != '"') {
            continue;
        }
        if (i + 1 >= close) {
            break;
        }
        meta->chars.push_back(json[i + 1]);
        const size_t end_quote = json.find('"', i + 1);
        if (end_quote == std::string::npos) {
            break;
        }
        i = end_quote;
    }

    double blank = 0.0;
    double sample_rate = 0.0;
    double fft_length = 0.0;
    double hop_length = 0.0;
    double stride = 1.0;
    double min_hz = 0.0;
    double max_hz = 0.0;
    double bins = 0.0;
    double classes = 0.0;
    double quantile = 0.9;
    std::string normalization;
    if (!extract_number(json, "blank_index", &blank) ||
        !extract_number(json, "sample_rate", &sample_rate) ||
        !extract_number(json, "fft_length", &fft_length) ||
        !extract_number(json, "hop_length", &hop_length) ||
        !extract_number(json, "spectrogram_min_freq_hz", &min_hz) ||
        !extract_number(json, "spectrogram_max_freq_hz", &max_hz) ||
        !extract_number(json, "spectrogram_frequency_bins", &bins) ||
        !extract_number(json, "num_classes", &classes) ||
        !extract_string(json, "normalization", &normalization) ||
        !extract_string(json, "onnx_input_name", &meta->input_name) ||
        !extract_string(json, "onnx_output_name", &meta->output_name)) {
        if (error) {
            *error = "DeepCW metadata is missing a required field";
        }
        return false;
    }
    extract_number(json, "output_time_stride", &stride);
    extract_number(json, "noise_level_quantile", &quantile);

    if (!(normalization.size() > 8 && normalization.compare(0, 7, "noise_q") == 0 &&
          normalization.size() >= 6 &&
          normalization.compare(normalization.size() - 6, 6, "_log1p") == 0)) {
        if (error) {
            *error = "Unsupported DeepCW normalization: " + normalization;
        }
        return false;
    }
    if (meta->chars.empty() || sample_rate < 100.0 || fft_length < 16.0 || hop_length < 1.0) {
        if (error) {
            *error = "DeepCW metadata values are not usable";
        }
        return false;
    }

    meta->blank_index = static_cast<int>(blank);
    meta->sample_rate = static_cast<int>(sample_rate);
    meta->fft_length = static_cast<int>(fft_length);
    meta->hop_length = static_cast<int>(hop_length);
    meta->output_time_stride = std::max(1, static_cast<int>(stride));
    meta->min_hz = min_hz;
    meta->max_hz = max_hz;
    meta->frequency_bins = static_cast<int>(bins);
    meta->noise_quantile = quantile;
    meta->num_classes = static_cast<int>(classes);
    return true;
}

void fft_radix2(std::vector<double> *real, std::vector<double> *imag)
{
    const int n = static_cast<int>(real->size());
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap((*real)[i], (*real)[j]);
            std::swap((*imag)[i], (*imag)[j]);
        }
    }
    for (int length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * M_PI / static_cast<double>(length);
        const double wlen_re = std::cos(angle);
        const double wlen_im = std::sin(angle);
        for (int i = 0; i < n; i += length) {
            double wr = 1.0;
            double wi = 0.0;
            const int half = length / 2;
            for (int k = 0; k < half; ++k) {
                const int a = i + k;
                const int b = a + half;
                const double tr = wr * (*real)[b] - wi * (*imag)[b];
                const double ti = wr * (*imag)[b] + wi * (*real)[b];
                (*real)[b] = (*real)[a] - tr;
                (*imag)[b] = (*imag)[a] - ti;
                (*real)[a] += tr;
                (*imag)[a] += ti;
                const double next_wr = wr * wlen_re - wi * wlen_im;
                wi = wr * wlen_im + wi * wlen_re;
                wr = next_wr;
            }
        }
    }
}

struct Span {
    char character;
    int start_frame;
    int end_frame;
};

struct LaneText {
    std::string text;
    bool prepend_space;
};

double smoothstep(double value)
{
    double t = value;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    return t * t * (3.0 - 2.0 * t);
}

void log_softmax_rows(const float *values, int rows, int cols, std::vector<float> *out)
{
    out->assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0.0f);
    for (int row = 0; row < rows; ++row) {
        const float *src = values + static_cast<size_t>(row) * static_cast<size_t>(cols);
        double peak = -1.0e300;
        for (int col = 0; col < cols; ++col) {
            peak = std::max(peak, static_cast<double>(src[col]));
        }
        double sum = 0.0;
        for (int col = 0; col < cols; ++col) {
            sum += std::exp(static_cast<double>(src[col]) - peak);
        }
        const double log_z = peak + std::log(std::max(sum, 1e-300));
        float *dst = out->data() + static_cast<size_t>(row) * static_cast<size_t>(cols);
        for (int col = 0; col < cols; ++col) {
            dst[col] = static_cast<float>(static_cast<double>(src[col]) - log_z);
        }
    }
}

std::vector<float> dilate_log_probs(const float *logits, int frames, int classes, int blank)
{
    std::vector<float> log_probs;
    log_softmax_rows(logits, frames, classes, &log_probs);
    const int radius = kStitchDilationFrames;
    std::vector<float> dilated(static_cast<size_t>(frames) * static_cast<size_t>(classes),
                               -1.0e30f);
    for (int frame = 0; frame < frames; ++frame) {
        for (int col = 0; col < classes; ++col) {
            double best = -1.0e300;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int source = frame + offset;
                if (source < 0 || source >= frames) {
                    continue;
                }
                best = std::max(best, static_cast<double>(
                                    log_probs[static_cast<size_t>(source) * classes + col]));
            }
            dilated[static_cast<size_t>(frame) * classes + col] = static_cast<float>(best);
        }
        dilated[static_cast<size_t>(frame) * classes + blank] =
            log_probs[static_cast<size_t>(frame) * classes + blank];
    }
    std::vector<float> normalized;
    log_softmax_rows(dilated.data(), frames, classes, &normalized);
    return normalized;
}

enum class PreviousSymbol { None, Char, Space };

struct Stitcher {
    std::vector<char> chars;
    int blank_index;
    int space_index;
    int class_count;
    int hop_samples;
    double frames_per_second;
    int64_t max_segment_samples;
    int left_context_frames;
    int freeze_guard_frames;
    int word_boundary_frames;

    int row_base;
    std::vector<float> sums;
    std::vector<float> weights;
    int covered_end;
    int frozen_end;
    int committed_end;
    PreviousSymbol previous_symbol;
    int blank_frames;
    bool has_committed_text;

    void clear()
    {
        row_base = 0;
        sums.clear();
        weights.clear();
        covered_end = 0;
        frozen_end = 0;
        committed_end = 0;
        previous_symbol = PreviousSymbol::None;
        blank_frames = 0;
        has_committed_text = false;
    }

    int context_start_frame() const
    {
        return std::max(0, frozen_end - left_context_frames);
    }

    int window_start_frame(int64_t end_sample) const
    {
        const double ratio =
            static_cast<double>(end_sample - max_segment_samples) / static_cast<double>(hop_samples);
        const int from_segment = static_cast<int>(std::ceil(ratio));
        return std::max(context_start_frame(), from_segment);
    }

    void ensure_rows(int end_frame)
    {
        const int have = static_cast<int>(weights.size());
        const int missing = end_frame - row_base - have;
        if (missing <= 0) {
            return;
        }
        sums.resize(sums.size() + static_cast<size_t>(missing) * static_cast<size_t>(class_count), 0.0f);
        weights.resize(weights.size() + static_cast<size_t>(missing), 0.0f);
    }

    void drop_rows_before(int frame)
    {
        int drop = frame - row_base;
        if (drop <= 0) {
            return;
        }
        drop = std::min(drop, static_cast<int>(weights.size()));
        sums.erase(sums.begin(),
                   sums.begin() + static_cast<std::ptrdiff_t>(drop) * class_count);
        weights.erase(weights.begin(), weights.begin() + drop);
        row_base += drop;
    }

    void add_window(int start_frame, const float *logits, int frame_count, bool is_final)
    {
        if (frame_count <= 0) {
            if (is_final) {
                frozen_end = covered_end;
            }
            return;
        }
        const int end_frame = start_frame + frame_count;
        ensure_rows(end_frame);
        const std::vector<float> log_probs =
            dilate_log_probs(logits, frame_count, class_count, blank_index);
        const int first = std::max(0, frozen_end - start_frame);
        for (int frame = first; frame < frame_count; ++frame) {
            const double left_seconds =
                (start_frame == 0) ? 1.0e300
                                   : static_cast<double>(frame) / frames_per_second;
            const double right_seconds =
                is_final ? 1.0e300
                         : static_cast<double>(frame_count - 1 - frame) / frames_per_second;
            const double weight = std::max(
                kMinViewWeight,
                smoothstep(left_seconds / kStitchLeftRampSeconds) *
                    smoothstep(right_seconds / kStitchRightRampSeconds));
            const int row = start_frame + frame - row_base;
            float *sum = sums.data() + static_cast<size_t>(row) * static_cast<size_t>(class_count);
            const float *src =
                log_probs.data() + static_cast<size_t>(frame) * static_cast<size_t>(class_count);
            for (int col = 0; col < class_count; ++col) {
                sum[col] += static_cast<float>(weight) * src[col];
            }
            weights[static_cast<size_t>(row)] += static_cast<float>(weight);
        }
        covered_end = std::max(covered_end, end_frame);
        if (is_final) {
            frozen_end = covered_end;
        } else {
            frozen_end = std::max(frozen_end, end_frame - freeze_guard_frames);
        }
    }

    std::vector<float> read_scores(int start_frame, int end_frame) const
    {
        const int count = end_frame - start_frame;
        std::vector<float> scores(static_cast<size_t>(count) * static_cast<size_t>(class_count),
                                  kUncoveredLogProb);
        for (int frame = 0; frame < count; ++frame) {
            scores[static_cast<size_t>(frame) * class_count + blank_index] = 0.0f;
            const int row = start_frame + frame - row_base;
            if (row < 0 || row >= static_cast<int>(weights.size())) {
                continue;
            }
            const float weight = weights[static_cast<size_t>(row)];
            if (!(weight > 0.0f)) {
                continue;
            }
            const float *sum = sums.data() + static_cast<size_t>(row) * static_cast<size_t>(class_count);
            float *dst = scores.data() + static_cast<size_t>(frame) * static_cast<size_t>(class_count);
            for (int col = 0; col < class_count; ++col) {
                dst[col] = sum[col] / weight;
            }
        }
        return scores;
    }
};

std::vector<int> argmax_rows(const std::vector<float> &scores, int frames, int classes)
{
    std::vector<int> best(static_cast<size_t>(frames), 0);
    for (int frame = 0; frame < frames; ++frame) {
        const float *row = scores.data() + static_cast<size_t>(frame) * static_cast<size_t>(classes);
        int index = 0;
        float peak = row[0];
        for (int col = 1; col < classes; ++col) {
            if (row[col] > peak) {
                peak = row[col];
                index = col;
            }
        }
        best[static_cast<size_t>(frame)] = index;
    }
    return best;
}

void decode_ctc_greedy(const std::vector<float> &scores, const Stitcher &stitcher,
                       PreviousSymbol previous_symbol, int blank_frames,
                       std::vector<Span> *spans, std::vector<int> *argmax_out)
{
    const int classes = stitcher.class_count;
    const int frames = classes > 0 ? static_cast<int>(scores.size() / static_cast<size_t>(classes)) : 0;
    std::vector<int> argmax = argmax_rows(scores, frames, classes);
    std::vector<int> indices = argmax;
    const int blank = stitcher.blank_index;
    const int space = stitcher.space_index;

    if (space >= 0) {
        int previous = -1;
        if (previous_symbol == PreviousSymbol::Char) {
            previous = -2;
        } else if (previous_symbol == PreviousSymbol::Space) {
            previous = space;
        }
        int frame = 0;
        while (frame < frames) {
            if (indices[static_cast<size_t>(frame)] != blank) {
                previous = indices[static_cast<size_t>(frame)];
                frame += 1;
                continue;
            }
            const int run_start = frame;
            while (frame < frames && indices[static_cast<size_t>(frame)] == blank) {
                frame += 1;
            }
            const int following = frame < frames ? indices[static_cast<size_t>(frame)] : -1;
            const int run_length = frame - run_start + (run_start == 0 ? blank_frames : 0);
            if (run_length >= stitcher.word_boundary_frames && previous != -1 && previous != space &&
                following >= 0 && following != space) {
                for (int at = run_start; at < frame; ++at) {
                    indices[static_cast<size_t>(at)] = space;
                }
            }
        }
    }

    spans->clear();
    int previous_index = -1;
    int active = -1;
    for (int frame = 0; frame < frames; ++frame) {
        const int index = indices[static_cast<size_t>(frame)];
        if (index == blank) {
            previous_index = -1;
            active = -1;
            continue;
        }
        if (index == previous_index) {
            if (active >= 0) {
                (*spans)[static_cast<size_t>(active)].end_frame = frame;
            }
            continue;
        }
        previous_index = index;
        if (index < 0 || index >= static_cast<int>(stitcher.chars.size())) {
            active = -1;
            continue;
        }
        Span span;
        span.character = stitcher.chars[static_cast<size_t>(index)];
        span.start_frame = frame;
        span.end_frame = frame;
        spans->push_back(span);
        active = static_cast<int>(spans->size()) - 1;
    }
    *argmax_out = std::move(argmax);
}

LaneText to_lane_text(const std::vector<Span> &spans, int begin, int end, bool prepend_space)
{
    LaneText lane;
    lane.prepend_space = prepend_space;
    bool pending_space = false;
    for (int i = begin; i < end; ++i) {
        const Span &span = spans[static_cast<size_t>(i)];
        if (span.character == ' ') {
            if (lane.text.empty()) {
                lane.prepend_space = true;
            } else {
                pending_space = true;
            }
            continue;
        }
        if (pending_space) {
            lane.text.push_back(' ');
            pending_space = false;
        }
        lane.text.push_back(span.character);
    }
    return lane;
}

struct Committed {
    bool any;
    LaneText text;
};

Committed take_text(Stitcher *stitcher, bool final)
{
    Committed result;
    result.any = false;
    const int start_frame = stitcher->committed_end;
    if (stitcher->covered_end - start_frame <= 0) {
        return result;
    }
    const bool starts_word =
        !stitcher->has_committed_text || stitcher->previous_symbol == PreviousSymbol::Space;
    const std::vector<float> scores =
        stitcher->read_scores(start_frame, stitcher->covered_end);
    std::vector<Span> spans;
    std::vector<int> argmax;
    decode_ctc_greedy(scores, *stitcher, stitcher->previous_symbol, stitcher->blank_frames, &spans,
                      &argmax);
    for (Span &span : spans) {
        span.start_frame += start_frame;
        span.end_frame += start_frame;
    }

    const int commit_limit = final ? 2000000000 : stitcher->frozen_end;
    int committed_count = 0;
    for (int index = 0; index < static_cast<int>(spans.size()); ++index) {
        const Span &span = spans[static_cast<size_t>(index)];
        if (span.end_frame + 1 >= commit_limit) {
            break;
        }
        if (span.character != ' ') {
            committed_count = index + 1;
        }
    }

    if (committed_count > 0) {
        result.any = true;
        result.text = to_lane_text(spans, 0, committed_count, starts_word);
        stitcher->has_committed_text = true;
        stitcher->committed_end = spans[static_cast<size_t>(committed_count - 1)].end_frame + 1;
        stitcher->previous_symbol = PreviousSymbol::Char;
        stitcher->blank_frames = 0;
    }

    int next_character = -1;
    for (int index = committed_count; index < static_cast<int>(spans.size()); ++index) {
        if (spans[static_cast<size_t>(index)].character != ' ') {
            next_character = index;
            break;
        }
    }
    const int limit = final ? stitcher->covered_end : stitcher->frozen_end;
    int release_end = limit - 1;
    if (next_character >= 0) {
        release_end = std::min(limit, spans[static_cast<size_t>(next_character)].start_frame) - 1;
    }

    PreviousSymbol symbol = stitcher->previous_symbol;
    int blanks = stitcher->blank_frames;
    for (int frame = stitcher->committed_end; frame < release_end; ++frame) {
        const int local = frame - start_frame;
        if (local >= 0 && local < static_cast<int>(argmax.size()) &&
            argmax[static_cast<size_t>(local)] == stitcher->space_index) {
            symbol = PreviousSymbol::Space;
            blanks = 0;
        } else {
            blanks += 1;
        }
    }
    if (release_end > stitcher->committed_end) {
        stitcher->previous_symbol = symbol;
        stitcher->blank_frames = blanks;
        stitcher->committed_end = release_end;
    }
    stitcher->drop_rows_before(stitcher->committed_end);
    return result;
}

std::pair<int, int> frequency_bin_range(int sample_rate, int fft_length, double min_hz, double max_hz)
{
    const double bin_hz = static_cast<double>(sample_rate) / static_cast<double>(fft_length);
    const int start_bin = static_cast<int>(std::ceil(min_hz / bin_hz));
    const int stop_bin = static_cast<int>(std::floor(max_hz / bin_hz)) + 1;
    return std::make_pair(start_bin, stop_bin);
}

bool audio_to_spectrogram(const float *audio, int length, const Metadata &meta,
                          std::vector<float> *spectrogram, int *frames_out, std::string *error)
{
    const int fft_length = meta.fft_length;
    const int hop = meta.hop_length;
    if (length < fft_length) {
        if (error) {
            *error = "Audio is too short for the DeepCW FFT";
        }
        return false;
    }
    const std::pair<int, int> bins =
        frequency_bin_range(meta.sample_rate, fft_length, meta.min_hz, meta.max_hz);
    const int start_bin = bins.first;
    const int stop_bin = bins.second;
    if (stop_bin - start_bin != meta.frequency_bins || start_bin < 0 || stop_bin > fft_length / 2 + 1) {
        if (error) {
            *error = "DeepCW spectrogram bin count does not match the metadata";
        }
        return false;
    }

    const int pad = fft_length / 2;
    std::vector<float> window(static_cast<size_t>(fft_length));
    for (int n = 0; n < fft_length; ++n) {
        window[static_cast<size_t>(n)] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(n) / static_cast<double>(fft_length)));
    }

    const int padded = length + 2 * pad;
    const int frames = 1 + (padded - fft_length) / hop;
    spectrogram->assign(static_cast<size_t>(frames) * static_cast<size_t>(meta.frequency_bins), 0.0f);
    std::vector<double> real(static_cast<size_t>(fft_length));
    std::vector<double> imag(static_cast<size_t>(fft_length));

    for (int frame = 0; frame < frames; ++frame) {
        const int start = frame * hop;
        for (int n = 0; n < fft_length; ++n) {
            const int index = reflect_index(start + n - pad, length);
            real[static_cast<size_t>(n)] =
                static_cast<double>(audio[index] * window[static_cast<size_t>(n)]);
            imag[static_cast<size_t>(n)] = 0.0;
        }
        fft_radix2(&real, &imag);
        float *dst = spectrogram->data() +
                     static_cast<size_t>(frame) * static_cast<size_t>(meta.frequency_bins);
        for (int bin = start_bin; bin < stop_bin; ++bin) {
            const double mag = std::sqrt(real[static_cast<size_t>(bin)] * real[static_cast<size_t>(bin)] +
                                         imag[static_cast<size_t>(bin)] * imag[static_cast<size_t>(bin)]);
            dst[bin - start_bin] = static_cast<float>(mag);
        }
    }

    std::vector<float> flat = *spectrogram;
    std::sort(flat.begin(), flat.end());
    const double rayleigh = std::sqrt(-2.0 * std::log(1.0 - meta.noise_quantile));
    const size_t last = flat.size() - 1;
    const size_t q_index =
        static_cast<size_t>(std::floor(meta.noise_quantile * static_cast<double>(last)));
    const double noise = std::max(static_cast<double>(flat[q_index]) / rayleigh,
                                   std::max(static_cast<double>(flat[last]) * kNoiseLevelMinRelativeToPeak,
                                            kNoiseLevelMin));
    const float scale = static_cast<float>(noise);
    for (float &bin : *spectrogram) {
        bin = std::log1p(bin / scale);
    }
    *frames_out = frames;
    return true;
}

const OrtApi *ort_api()
{
    return OrtGetApiBase()->GetApi(ORT_API_VERSION);
}

std::string ort_message(OrtStatus *status)
{
    if (!status) {
        return std::string();
    }
    const char *text = ort_api()->GetErrorMessage(status);
    std::string message = text ? text : "ONNX Runtime error";
    ort_api()->ReleaseStatus(status);
    return message;
}

}  // namespace

struct DeepCwDecoder {
    Metadata meta;
    Stitcher stitcher;
    std::string transcript;
    std::vector<float> audio;
    int64_t audio_origin;
    int64_t next_end;
    bool have_next;
    int64_t step_samples;
    int64_t min_pending_samples;
    std::string error;

    const OrtApi *api;
    OrtEnv *env;
    OrtSessionOptions *options;
    OrtSession *session;
    OrtMemoryInfo *memory;

    std::string run_window(int64_t start_sample, int64_t end_sample, bool is_final, std::string *error);
    std::string step(int64_t end_sample, std::string *error);
    void drop_audio_before(int64_t sample);
};

std::string DeepCwDecoder::run_window(int64_t start_sample, int64_t end_sample, bool is_final,
                                      std::string *error)
{
    const int hop = stitcher.hop_samples;
    int start_frame = 0;
    if (hop > 0) {
        start_frame = static_cast<int>(start_sample / hop);
    }
    if (end_sample < start_sample) {
        end_sample = start_sample;
    }
    const int64_t slice_begin = start_sample - audio_origin;
    const int64_t slice_end = end_sample - audio_origin;
    if (slice_begin < 0 || slice_end > static_cast<int64_t>(audio.size())) {
        if (error) {
            *error = "DeepCW audio window is outside the retained buffer";
        }
        return std::string();
    }
    const int slice_count = static_cast<int>(slice_end - slice_begin);
    std::vector<float> logits;
    int time_steps = 0;
    if (slice_count >= meta.fft_length) {
        std::vector<float> spectrogram;
        int spec_frames = 0;
        if (!audio_to_spectrogram(audio.data() + slice_begin, slice_count, meta, &spectrogram,
                                  &spec_frames, error)) {
            return std::string();
        }
        const int64_t shape[4] = {1, 1, spec_frames, meta.frequency_bins};
        OrtValue *input = nullptr;
        OrtStatus *status = api->CreateTensorWithDataAsOrtValue(
            memory, spectrogram.data(), spectrogram.size() * sizeof(float), shape, 4,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
        if (status) {
            if (error) {
                *error = ort_message(status);
            }
            return std::string();
        }
        const char *input_names[] = {meta.input_name.c_str()};
        const char *output_names[] = {meta.output_name.c_str()};
        OrtValue *output = nullptr;
        status = api->Run(session, nullptr, input_names, &input, 1, output_names, 1, &output);
        api->ReleaseValue(input);
        if (status) {
            if (error) {
                *error = ort_message(status);
            }
            return std::string();
        }
        OrtTensorTypeAndShapeInfo *info = nullptr;
        status = api->GetTensorTypeAndShape(output, &info);
        if (status) {
            api->ReleaseValue(output);
            if (error) {
                *error = ort_message(status);
            }
            return std::string();
        }
        size_t dims_count = 0;
        status = api->GetDimensionsCount(info, &dims_count);
        if (status) {
            api->ReleaseTensorTypeAndShapeInfo(info);
            api->ReleaseValue(output);
            if (error) {
                *error = ort_message(status);
            }
            return std::string();
        }
        std::vector<int64_t> dims(dims_count);
        if (dims_count > 0) {
            status = api->GetDimensions(info, dims.data(), dims_count);
            if (status) {
                api->ReleaseTensorTypeAndShapeInfo(info);
                api->ReleaseValue(output);
                if (error) {
                    *error = ort_message(status);
                }
                return std::string();
            }
        }
        api->ReleaseTensorTypeAndShapeInfo(info);
        float *data = nullptr;
        status = api->GetTensorMutableData(output, reinterpret_cast<void **>(&data));
        if (status || !data) {
            api->ReleaseValue(output);
            if (error && status) {
                *error = ort_message(status);
            }
            return std::string();
        }
        int64_t classes = meta.num_classes;
        int64_t time = 0;
        if (dims_count == 3 && dims[2] == classes) {
            time = dims[1];
        } else if (dims_count == 2 && dims[1] == classes) {
            time = dims[0];
        } else {
            api->ReleaseValue(output);
            if (error) {
                *error = "DeepCW model output shape is not [batch, time, class]";
            }
            return std::string();
        }
        time_steps = static_cast<int>(time);
        logits.assign(data, data + time * classes);
        api->ReleaseValue(output);
    }

    stitcher.add_window(start_frame, logits.empty() ? nullptr : logits.data(), time_steps, is_final);
    const Committed committed = take_text(&stitcher, is_final);
    if (!committed.any || committed.text.text.empty()) {
        return std::string();
    }
    std::string piece;
    if (committed.text.prepend_space && !transcript.empty()) {
        piece.push_back(' ');
    }
    piece += committed.text.text;
    transcript += piece;
    return piece;
}

std::string DeepCwDecoder::step(int64_t end_sample, std::string *error)
{
    const int start_frame = stitcher.window_start_frame(end_sample);
    const int64_t start_sample = static_cast<int64_t>(start_frame) * stitcher.hop_samples;
    return run_window(start_sample, end_sample, false, error);
}

void DeepCwDecoder::drop_audio_before(int64_t sample)
{
    if (sample <= audio_origin) {
        return;
    }
    const int64_t drop = std::min(sample - audio_origin, static_cast<int64_t>(audio.size()));
    audio.erase(audio.begin(), audio.begin() + static_cast<std::ptrdiff_t>(drop));
    audio_origin += drop;
}

bool deepcw_metadata_audio(const char *metadata_path, double *sample_rate, double *center_hz,
                           std::string *error)
{
    Metadata meta;
    if (!load_metadata(metadata_path, &meta, error)) {
        return false;
    }
    if (sample_rate) {
        *sample_rate = meta.sample_rate;
    }
    if (center_hz) {
        *center_hz = 0.5 * (meta.min_hz + meta.max_hz);
    }
    return true;
}

DeepCwDecoder *deepcw_decoder_create(const char *model_path, const char *metadata_path,
                                     std::string *error)
{
    Metadata meta;
    if (!load_metadata(metadata_path, &meta, error)) {
        return nullptr;
    }
    DeepCwDecoder *decoder = new DeepCwDecoder();
    decoder->meta = meta;
    decoder->audio_origin = 0;
    decoder->next_end = 0;
    decoder->have_next = false;
    decoder->env = nullptr;
    decoder->options = nullptr;
    decoder->session = nullptr;
    decoder->memory = nullptr;
    decoder->api = ort_api();

    decoder->stitcher.chars = meta.chars;
    decoder->stitcher.blank_index = meta.blank_index;
    decoder->stitcher.space_index = -1;
    for (int i = 0; i < static_cast<int>(meta.chars.size()); ++i) {
        if (meta.chars[static_cast<size_t>(i)] == ' ') {
            decoder->stitcher.space_index = i;
        }
    }
    decoder->stitcher.class_count = meta.num_classes;
    decoder->stitcher.hop_samples = meta.hop_length * meta.output_time_stride;
    decoder->stitcher.frames_per_second =
        static_cast<double>(meta.sample_rate) / static_cast<double>(decoder->stitcher.hop_samples);
    decoder->stitcher.max_segment_samples = python_int(kMaxSegmentSeconds * meta.sample_rate);
    decoder->stitcher.left_context_frames = static_cast<int>(std::lround(
        kStitchLeftContextSeconds * meta.sample_rate / decoder->stitcher.hop_samples));
    decoder->stitcher.freeze_guard_frames = static_cast<int>(std::lround(
        kStitchFreezeGuardSeconds * meta.sample_rate / decoder->stitcher.hop_samples));
    decoder->stitcher.word_boundary_frames = static_cast<int>(
        std::ceil(static_cast<double>(kWordBoundaryBlankHops) / meta.output_time_stride));
    decoder->stitcher.clear();
    decoder->step_samples = python_int(kStepSeconds * meta.sample_rate);
    decoder->min_pending_samples = python_int(kMinPendingSeconds * meta.sample_rate);

    OrtStatus *status = decoder->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "deepcw", &decoder->env);
    if (status) {
        if (error) {
            *error = ort_message(status);
        }
        deepcw_decoder_destroy(decoder);
        return nullptr;
    }
    status = decoder->api->CreateSessionOptions(&decoder->options);
    if (status) {
        if (error) {
            *error = ort_message(status);
        }
        deepcw_decoder_destroy(decoder);
        return nullptr;
    }
    status = decoder->api->SetIntraOpNumThreads(decoder->options, 2);
    if (status) {
        ort_message(status);
    }
    status = decoder->api->SetSessionGraphOptimizationLevel(decoder->options, ORT_ENABLE_ALL);
    if (status) {
        ort_message(status);
    }
    status = decoder->api->CreateSession(decoder->env, model_path, decoder->options, &decoder->session);
    if (status) {
        if (error) {
            *error = ort_message(status);
        }
        deepcw_decoder_destroy(decoder);
        return nullptr;
    }
    status = decoder->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &decoder->memory);
    if (status) {
        if (error) {
            *error = ort_message(status);
        }
        deepcw_decoder_destroy(decoder);
        return nullptr;
    }
    return decoder;
}

void deepcw_decoder_destroy(DeepCwDecoder *decoder)
{
    if (!decoder) {
        return;
    }
    const OrtApi *api = decoder->api ? decoder->api : ort_api();
    if (decoder->memory) {
        api->ReleaseMemoryInfo(decoder->memory);
    }
    if (decoder->session) {
        api->ReleaseSession(decoder->session);
    }
    if (decoder->options) {
        api->ReleaseSessionOptions(decoder->options);
    }
    if (decoder->env) {
        api->ReleaseEnv(decoder->env);
    }
    delete decoder;
}

void deepcw_decoder_reset(DeepCwDecoder *decoder)
{
    if (!decoder) {
        return;
    }
    decoder->stitcher.clear();
    decoder->transcript.clear();
    decoder->audio.clear();
    decoder->audio_origin = 0;
    decoder->next_end = 0;
    decoder->have_next = false;
    decoder->error.clear();
}

void deepcw_decoder_process(DeepCwDecoder *decoder, const float *samples, int count,
                            std::string *newly)
{
    if (!decoder || !samples || count <= 0 || !newly) {
        return;
    }
    decoder->audio.insert(decoder->audio.end(), samples, samples + count);
    if (!decoder->have_next) {
        decoder->next_end = decoder->min_pending_samples;
        decoder->have_next = true;
    }
    const int64_t length = decoder->audio_origin + static_cast<int64_t>(decoder->audio.size());
    while (decoder->next_end < length) {
        std::string error;
        *newly += decoder->step(decoder->next_end, &error);
        if (!error.empty()) {
            decoder->error = error;
        }
        decoder->next_end += decoder->step_samples;
        const int64_t keep =
            static_cast<int64_t>(decoder->stitcher.context_start_frame()) * decoder->stitcher.hop_samples;
        decoder->drop_audio_before(keep);
    }
}

void deepcw_decoder_finish(DeepCwDecoder *decoder, std::string *newly)
{
    if (!decoder || !newly) {
        return;
    }
    const int64_t length = decoder->audio_origin + static_cast<int64_t>(decoder->audio.size());
    for (int guard = 0; guard < 64; ++guard) {
        std::string error;
        const int start_frame = decoder->stitcher.context_start_frame();
        int64_t end_sample = static_cast<int64_t>(start_frame) * decoder->stitcher.hop_samples +
                             decoder->stitcher.max_segment_samples;
        if (end_sample > length) {
            end_sample = length;
        }
        const bool is_final = end_sample >= length;
        const int64_t start_sample =
            static_cast<int64_t>(start_frame) * decoder->stitcher.hop_samples;
        *newly += decoder->run_window(start_sample, end_sample, is_final, &error);
        if (!error.empty()) {
            decoder->error = error;
        }
        if (is_final) {
            break;
        }
    }
}

const std::string &deepcw_decoder_error(const DeepCwDecoder *decoder)
{
    static const std::string empty;
    return decoder ? decoder->error : empty;
}

const std::string &deepcw_decoder_text(const DeepCwDecoder *decoder)
{
    static const std::string empty;
    return decoder ? decoder->transcript : empty;
}

double deepcw_decoder_sample_rate(const DeepCwDecoder *decoder)
{
    return decoder ? decoder->meta.sample_rate : 0.0;
}

double deepcw_decoder_center_hz(const DeepCwDecoder *decoder)
{
    return decoder ? 0.5 * (decoder->meta.min_hz + decoder->meta.max_hz) : 0.0;
}

struct DeepCwFront {
    double src_rate;
    double pitch_hz;
    int real_audio;
    double target_rate;
    double center_hz;
    double mix_phase;
    std::vector<float> fir;
    std::vector<float> delay;
    int delay_at;
    int delay_fill;
    std::vector<float> source;
    int64_t source_origin;
    double out_pos;
};

static void design_lowpass(std::vector<float> *fir, double cutoff_hz, double sample_rate)
{
    const int taps = 1201;
    fir->assign(static_cast<size_t>(taps), 0.0f);
    const int mid = taps / 2;
    const double fc = cutoff_hz / sample_rate;
    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        const int k = n - mid;
        const double hann = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (taps - 1)));
        const double sinc = (k == 0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * k) / (M_PI * k);
        const double coeff = sinc * hann;
        (*fir)[static_cast<size_t>(n)] = static_cast<float>(coeff);
        sum += coeff;
    }
    if (sum != 0.0) {
        for (float &coeff : *fir) {
            coeff = static_cast<float>(static_cast<double>(coeff) / sum);
        }
    }
}

DeepCwFront *deepcw_front_create(double src_rate, double pitch_hz, int real_audio,
                                 double target_rate, double center_hz)
{
    if (!(src_rate > 0.0) || !(target_rate > 0.0)) {
        return nullptr;
    }
    DeepCwFront *front = new DeepCwFront();
    front->src_rate = src_rate;
    front->pitch_hz = pitch_hz;
    front->real_audio = real_audio ? 1 : 0;
    front->target_rate = target_rate;
    front->center_hz = center_hz;
    front->mix_phase = 0.0;
    front->delay_at = 0;
    front->delay_fill = 0;
    front->source_origin = 0;
    front->out_pos = 0.0;
    const double cutoff = std::min(0.45 * target_rate, std::max(200.0, center_hz + 700.0));
    design_lowpass(&front->fir, cutoff, src_rate);
    front->delay.assign(front->fir.size(), 0.0f);
    return front;
}

void deepcw_front_destroy(DeepCwFront *front)
{
    delete front;
}

void deepcw_front_reset(DeepCwFront *front, double src_rate, double pitch_hz, int real_audio)
{
    if (!front) {
        return;
    }
    const bool rebuild = std::fabs(src_rate - front->src_rate) > 0.5;
    front->src_rate = src_rate;
    front->pitch_hz = pitch_hz;
    front->real_audio = real_audio ? 1 : 0;
    front->mix_phase = 0.0;
    front->delay_at = 0;
    front->delay_fill = 0;
    std::fill(front->delay.begin(), front->delay.end(), 0.0f);
    front->source.clear();
    front->source_origin = 0;
    front->out_pos = 0.0;
    if (rebuild) {
        const double cutoff =
            std::min(0.45 * front->target_rate, std::max(200.0, front->center_hz + 700.0));
        design_lowpass(&front->fir, cutoff, src_rate);
        front->delay.assign(front->fir.size(), 0.0f);
    }
}

void deepcw_front_process(DeepCwFront *front, const float *interleaved, int complex_count,
                          std::vector<float> *pcm)
{
    if (!front || !interleaved || complex_count <= 0 || !pcm) {
        return;
    }
    const double step = 2.0 * M_PI * (front->pitch_hz - front->center_hz) / front->src_rate;
    const int taps = static_cast<int>(front->fir.size());
    for (int n = 0; n < complex_count; ++n) {
        const float i = interleaved[2 * n];
        const float q = interleaved[2 * n + 1];
        const double angle = front->mix_phase;
        const float mixed = front->real_audio
                                ? static_cast<float>(static_cast<double>(i) * std::cos(angle))
                                : static_cast<float>(static_cast<double>(i) * std::cos(angle) +
                                                     static_cast<double>(q) * std::sin(angle));
        front->mix_phase += step;
        if (front->mix_phase > 8.0 * M_PI || front->mix_phase < -8.0 * M_PI) {
            front->mix_phase = std::fmod(front->mix_phase, 2.0 * M_PI);
        }

        front->delay[static_cast<size_t>(front->delay_at)] = mixed;
        front->delay_at += 1;
        if (front->delay_at >= taps) {
            front->delay_at = 0;
        }
        if (front->delay_fill < taps) {
            front->delay_fill += 1;
        }
        if (front->delay_fill < taps) {
            continue;
        }
        double acc = 0.0;
        int tap = front->delay_at;
        for (int k = 0; k < taps; ++k) {
            if (tap >= taps) {
                tap = 0;
            }
            acc += static_cast<double>(front->delay[static_cast<size_t>(tap)]) *
                   static_cast<double>(front->fir[static_cast<size_t>(k)]);
            tap += 1;
        }
        front->source.push_back(static_cast<float>(acc));
    }

    const double ratio = front->src_rate / front->target_rate;
    const int64_t available = front->source_origin + static_cast<int64_t>(front->source.size());
    while (front->out_pos + 1.0 < static_cast<double>(available)) {
        const int64_t left = static_cast<int64_t>(std::floor(front->out_pos));
        const double fraction = front->out_pos - static_cast<double>(left);
        const int64_t i0 = left - front->source_origin;
        const int64_t i1 = i0 + 1;
        if (i0 < 0 || i1 >= static_cast<int64_t>(front->source.size())) {
            break;
        }
        const float sample = static_cast<float>(
            front->source[static_cast<size_t>(i0)] * (1.0 - fraction) +
            front->source[static_cast<size_t>(i1)] * fraction);
        pcm->push_back(sample);
        front->out_pos += ratio;
    }
    const int64_t keep = static_cast<int64_t>(std::floor(front->out_pos));
    if (keep > front->source_origin) {
        const int64_t drop =
            std::min(keep - front->source_origin, static_cast<int64_t>(front->source.size()));
        front->source.erase(front->source.begin(),
                            front->source.begin() + static_cast<std::ptrdiff_t>(drop));
        front->source_origin += drop;
    }
}
