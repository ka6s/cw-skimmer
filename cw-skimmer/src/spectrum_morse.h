/**
 * Headless CW copy from one spectrum column.
 *
 * This is the decision and timing engine from
 * gui/spectrummorsewindow.cpp, without the Qt widgets. A column is key-down
 * when the tuned bin is a lit trace (noise + 16 dB) and stays down until
 * noise + 12.5 dB. A character is copied only after six dahs of newer
 * spectrum have arrived.
 */

#ifndef SPECTRUM_MORSE_H
#define SPECTRUM_MORSE_H

#include <string>
#include <vector>

class SpectrumMorse {
public:
  SpectrumMorse();

  void reset();

  /**
   * One waterfall column at the tuned frequency.
   * power_db and noise_floor_db use the same scale.
   * bin_width_hz is the column spacing in Hz. When it is above 20 Hz the
   * column period is 1/bin_width_hz, matching the spectrum window.
   */
  void feed(float power_db, float noise_floor_db, float bin_width_hz);

  const std::string &text() const { return text_; }
  double lagSeconds() const;
  double wpm() const;
  bool calibrated() const { return unit_calibrated_; }

private:
  struct Column {
    double t_sec;
    bool on;
  };
  struct Run {
    bool on;
    double sec;
  };
  struct Timing {
    double dit;
    double dah_cut;
    double letter_cut;
    double word_cut;
    bool ok;
  };
  void pushRun(bool on, double seconds);
  void sealOldest();
  Timing measure(const std::vector<Run> &runs) const;
  std::string decodeRuns(const std::vector<Run> &runs, const Timing &timing) const;
  void publish(double cursor_sec);
  void setText(const std::string &text);

  std::vector<Column> hist_;
  int next_;
  std::vector<Run> done_;
  std::string text_;
  std::string sealed_;

  double now_sec_;
  bool have_state_;
  bool is_on_;
  bool saw_mark_;
  double state_start_sec_;
  double unit_seconds_;
  bool unit_calibrated_;
  int columns_;
  double column_sec_;
  bool latched_on_;
};

#endif
