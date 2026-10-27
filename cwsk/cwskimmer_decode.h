#ifndef CWSKIMMER_DECODE_H
#define CWSKIMMER_DECODE_H

/*
 * Single-channel Morse decoder lifted from CW Skimmer 2.1.0.410
 * (CwSkimmer.exe, Alex Shovkoplyas VE3NEA).
 *
 * The routines follow the unpacked image, not a generic decoder:
 *   complex NCO (configured pitch is negated, as in TDspProcessor.SetUp)
 *   512-tap lowpass = 4-term Blackman-Harris window times the binary sinc
 *   93.75 Hz frames (6000 Hz / 64), the rate implied by constant K = 112.5
 *   two-state explicit-duration HMM (TElementExtractor + ProbOfLen)
 *   log-normal element model (TMorseElementStats)
 *   64-entry pattern table, first exact match (TMorseDecoder)
 *
 * Wideband context, not run per sample here: IQ balance, octave/polyphase
 * resample onto a 6000 Hz (or bandwidth/3000) stream, then a 64-bin
 * filter bank whose prototype is the same window. cw_analysis_prototype()
 * builds that prototype. One decoder instance is one tuned channel.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CwDecoder CwDecoder;

/* bandwidth_hz is clamped to the binary's 20..700 Hz main-filter range.
   iq != 0 selects interleaved I,Q samples; pitch_hz is the LO (negated
   internally). pitch_hz == 0 leaves a complex baseband stream unmixed. */
CwDecoder *cw_decoder_create(double sample_rate, double pitch_hz,
                             double bandwidth_hz, int iq);
void cw_decoder_destroy(CwDecoder *d);

/* Consume samples. Writes newly committed characters into out (NUL
   terminated when out_cap > 0) and returns the count written, not
   counting the NUL. Characters still inside an open element stay
   buffered until cw_decoder_finish or until later frames close them. */
int cw_decoder_process(CwDecoder *d, const float *samples, int n,
                       char *out, int out_cap);

/* Flush the open element and copy any text not yet returned. */
int cw_decoder_finish(CwDecoder *d, char *out, int out_cap);

/* Weighted dit/dah/intra estimate. Seed tables give about 23 WPM. */
double cw_decoder_wpm(const CwDecoder *d);

/* Whole text committed so far, stable for the life of the decoder. */
const char *cw_decoder_text(const CwDecoder *d);

/* Analysis prototype used by the wideband bank: N taps, cutoff_hz at
   sample_rate. Same Blackman-Harris and sinc as MakeMainFilter / the
   DSP setup window. Taps are unity-DC-gain. */
void cw_analysis_prototype(float *taps, int n, double sample_rate,
                           double cutoff_hz);

#define CW_FRAME_HZ 93.75
#define CW_INTERNAL_HZ 6000.0
#define CW_FIR_TAPS 512

#ifdef __cplusplus
}
#endif

#endif
