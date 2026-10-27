/**
 * Decode a synthetic 800 Hz Morse tone with the DeepCW engine, then the
 * same text carried as complex baseband at 48 kHz.
 */

#include "deepcw_engine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *morse_of(char c)
{
    switch (c) {
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
    default: return nullptr;
    }
}

static void add_silence(std::vector<float> *out, int n)
{
    out->insert(out->end(), static_cast<size_t>(n), 0.0f);
}

static void add_mark(std::vector<float> *envelope, int n, int edge)
{
    for (int i = 0; i < n; ++i) {
        double env = 1.0;
        if (i < edge) {
            env = 0.5 - 0.5 * std::cos(M_PI * i / edge);
        } else if (i > n - edge) {
            env = 0.5 - 0.5 * std::cos(M_PI * (n - 1 - i) / edge);
        }
        envelope->push_back(static_cast<float>(env));
    }
}

static std::vector<float> keyed_envelope(const char *text, double sample_rate, double wpm)
{
    const double unit = 1.2 / wpm;
    const int u = std::max(1, static_cast<int>(std::lround(unit * sample_rate)));
    const int edge = std::max(1, static_cast<int>(0.005 * sample_rate));
    std::vector<float> out;
    add_silence(&out, static_cast<int>(0.5 * sample_rate));
    bool word = false;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ') {
            add_silence(&out, 7 * u);
            word = true;
            continue;
        }
        const char *pattern = morse_of(*p);
        if (!pattern) {
            continue;
        }
        if (p != text && !word) {
            add_silence(&out, 3 * u);
        }
        word = false;
        for (const char *e = pattern; *e; ++e) {
            add_mark(&out, (*e == '-') ? 3 * u : u, edge);
            if (e[1]) {
                add_silence(&out, u);
            }
        }
    }
    add_silence(&out, static_cast<int>(1.0 * sample_rate));
    return out;
}

static std::vector<float> render_real(const std::vector<float> &envelope, double sample_rate,
                                      double freq, double amp)
{
    std::vector<float> out(envelope.size());
    double phase = 0.0;
    const double step = 2.0 * M_PI * freq / sample_rate;
    for (size_t i = 0; i < envelope.size(); ++i) {
        out[i] = static_cast<float>(amp * envelope[i] * std::sin(phase));
        phase += step;
    }
    return out;
}

static void write_f32(const char *path, const std::vector<float> &pcm)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(float)));
}

static int decode_pcm(DeepCwDecoder *decoder, const std::vector<float> &pcm, std::string *text)
{
    const int chunk = 3200;
    for (size_t at = 0; at < pcm.size(); at += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(pcm.size() - at, static_cast<size_t>(chunk)));
        deepcw_decoder_process(decoder, pcm.data() + at, n, text);
    }
    deepcw_decoder_finish(decoder, text);
    if (!deepcw_decoder_error(decoder).empty()) {
        std::fprintf(stderr, "engine error: %s\n", deepcw_decoder_error(decoder).c_str());
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *model = argc > 1 ? argv[1] : "models/model.onnx";
    const char *meta = argc > 2 ? argv[2] : "models/model.onnx.json";
    std::string error;
    DeepCwDecoder *decoder = deepcw_decoder_create(model, meta, &error);
    if (!decoder) {
        std::fprintf(stderr, "create failed: %s\n", error.c_str());
        return 1;
    }

    const char *expect = "CQ DE TEST";
    const std::vector<float> envelope = keyed_envelope(expect, 3200.0, 20.0);
    const std::vector<float> audio = render_real(envelope, 3200.0, 800.0, 0.25);
    write_f32("/tmp/deepcw_pcm.f32", audio);
    std::string text;
    if (decode_pcm(decoder, audio, &text) != 0) {
        deepcw_decoder_destroy(decoder);
        return 1;
    }
    std::printf("direct: [%s]\n", text.c_str());

    deepcw_decoder_reset(decoder);
    const double src_rate = 48000.0;
    const double tone_hz = 2500.0;
    const std::vector<float> wide_env = keyed_envelope(expect, src_rate, 20.0);
    std::vector<float> iq(wide_env.size() * 2);
    {
        double phase = 0.0;
        const double step = 2.0 * M_PI * tone_hz / src_rate;
        for (size_t i = 0; i < wide_env.size(); ++i) {
            iq[2 * i] = static_cast<float>(wide_env[i] * std::cos(phase));
            iq[2 * i + 1] = static_cast<float>(wide_env[i] * std::sin(phase));
            phase += step;
        }
    }
    DeepCwFront *front = deepcw_front_create(src_rate, tone_hz, 0, 3200.0, 800.0);
    std::vector<float> pcm;
    const int block = 1024;
    for (size_t at = 0; at < wide_env.size(); at += static_cast<size_t>(block)) {
        const int n = static_cast<int>(std::min(wide_env.size() - at, static_cast<size_t>(block)));
        deepcw_front_process(front, iq.data() + at * 2, n, &pcm);
    }
    write_f32("/tmp/deepcw_front.f32", pcm);
    std::string shifted;
    if (decode_pcm(decoder, pcm, &shifted) != 0) {
        deepcw_front_destroy(front);
        deepcw_decoder_destroy(decoder);
        return 1;
    }
    std::printf("shifted: [%s]\n", shifted.c_str());
    deepcw_front_destroy(front);
    deepcw_decoder_destroy(decoder);

    const bool direct_ok = text.find("CQ") != std::string::npos && text.find("TEST") != std::string::npos;
    const bool shifted_ok =
        shifted.find("CQ") != std::string::npos && shifted.find("TEST") != std::string::npos;
    if (!direct_ok || !shifted_ok) {
        std::fprintf(stderr, "expected the text to contain CQ and TEST\n");
        return 1;
    }
    return 0;
}
