#ifndef DEEPCW_ENGINE_H
#define DEEPCW_ENGINE_H

/*
 * DeepCW engine, following examples/python/decode_morse.py in
 * https://github.com/e04/deepcw-engine (AGPL-3.0).
 *
 * The model reads mono audio at metadata sample_rate (3200 Hz for the
 * published model). It does not read IQ. deepcw_front_* turns a radio
 * chunk into that audio: the selected tone is shifted to the middle of
 * the model's spectrogram band, low-passed, and resampled.
 */

#include <string>
#include <vector>

struct DeepCwDecoder;

/* Read sample_rate and the center of the spectrogram band. */
bool deepcw_metadata_audio(const char *metadata_path, double *sample_rate,
                           double *center_hz, std::string *error);

DeepCwDecoder *deepcw_decoder_create(const char *model_path,
                                     const char *metadata_path,
                                     std::string *error);
void deepcw_decoder_destroy(DeepCwDecoder *decoder);
void deepcw_decoder_reset(DeepCwDecoder *decoder);

/* Append 3200 Hz mono samples. Newly committed characters are appended
 * to `newly` (the string is not cleared). */
void deepcw_decoder_process(DeepCwDecoder *decoder, const float *samples, int count,
                            std::string *newly);
void deepcw_decoder_finish(DeepCwDecoder *decoder, std::string *newly);

const std::string &deepcw_decoder_text(const DeepCwDecoder *decoder);
const std::string &deepcw_decoder_error(const DeepCwDecoder *decoder);
double deepcw_decoder_sample_rate(const DeepCwDecoder *decoder);
double deepcw_decoder_center_hz(const DeepCwDecoder *decoder);

struct DeepCwFront;

/* real_audio != 0: interleaved samples are real audio in I and Q = 0.
 * Otherwise they are complex baseband. pitch_hz is the true tone
 * frequency. Output is mono at target_rate, with that tone at center_hz. */
DeepCwFront *deepcw_front_create(double src_rate, double pitch_hz, int real_audio,
                                 double target_rate, double center_hz);
void deepcw_front_destroy(DeepCwFront *front);
void deepcw_front_reset(DeepCwFront *front, double src_rate, double pitch_hz,
                        int real_audio);
void deepcw_front_process(DeepCwFront *front, const float *interleaved,
                          int complex_count, std::vector<float> *pcm);

#endif
