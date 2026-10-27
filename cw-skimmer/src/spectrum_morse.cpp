/**
 * Spectrum-column Morse copy.
 *
 * Port of the timing in gui/spectrummorsewindow.cpp so deskHPSDR can run
 * the same decoder without linking Qt. Keep the thresholds, the six-dah
 * lag, and the histogram timing together with that file.
 */

#include "spectrum_morse.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

double medianOf(std::vector<double> v) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  if (n % 2 == 1) {
    return v[n / 2];
  }
  return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

char morseChar(const std::string &pattern) {
  static const struct {
    const char *pat;
    char ch;
  } table[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},
    {".", 'E'},     {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},
    {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'},   {".-..", 'L'},
    {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},   {".--.", 'P'},
    {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},
    {"-.--", 'Y'},  {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
    {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
    {"---..", '8'}, {"----.", '9'},
  };
  for (const auto &e : table) {
    if (pattern == e.pat) {
      return e.ch;
    }
  }
  return '\0';
}

/* Same ramp as SpectrumWidget::powerToColor. White trace is noise+16 dB.
 * Once the key is down it stays down until noise+12.5 dB. */
constexpr float kTraceAttackDb = 16.0f;
constexpr float kTraceReleaseDb = 12.5f;

bool spectrumTraceOn(float power_db, float noise_floor_db, bool latched_on) {
  if (!std::isfinite(power_db)) {
    return false;
  }
  const float floor_db = std::max(-110.0f, noise_floor_db);
  const float need = latched_on ? kTraceReleaseDb : kTraceAttackDb;
  return power_db >= floor_db + need;
}

double columnPeriodSec(float bin_width_hz) {
  if (bin_width_hz > 20.0f) {
    return 1.0 / static_cast<double>(bin_width_hz);
  }
  return 0.0107;
}

struct LengthBin {
  int count;
  double lo;
  double hi;
  LengthBin() : count(0), lo(0.0), hi(0.0) {}
};

int lengthBin(double sec, double quantum) {
  const int b = static_cast<int>(std::lround(sec / quantum));
  return b < 1 ? 1 : b;
}

void noteBin(std::vector<LengthBin> &hist, int bin, double sec) {
  if (bin >= static_cast<int>(hist.size())) {
    hist.resize(static_cast<std::size_t>(bin) + 1);
  }
  LengthBin &slot = hist[static_cast<std::size_t>(bin)];
  if (slot.count == 0) {
    slot.lo = sec;
    slot.hi = sec;
  } else {
    slot.lo = std::min(slot.lo, sec);
    slot.hi = std::max(slot.hi, sec);
  }
  ++slot.count;
}

std::vector<int> histogramPeaks(const std::vector<LengthBin> &hist, int total) {
  std::vector<int> peaks;
  if (total <= 0 || hist.empty()) {
    return peaks;
  }
  const int thresh = std::max(3, static_cast<int>(std::lround(0.05 * total)));
  int first = -1;
  int last = -1;
  for (int i = 0; i < static_cast<int>(hist.size()); ++i) {
    if (hist[static_cast<std::size_t>(i)].count > 0) {
      if (first < 0) {
        first = i;
      }
      last = i;
    }
  }
  if (first < 0) {
    return peaks;
  }
  for (int k = first; k <= last; ++k) {
    const int count = hist[static_cast<std::size_t>(k)].count;
    if (count < thresh) {
      continue;
    }
    const int left = (k > 0) ? hist[static_cast<std::size_t>(k - 1)].count : 0;
    const int right = (k + 1 < static_cast<int>(hist.size()))
                      ? hist[static_cast<std::size_t>(k + 1)].count : 0;
    if (count >= left && count > right) {
      peaks.push_back(k);
    }
  }
  if (peaks.empty()) {
    int best = first;
    for (int k = first; k <= last; ++k) {
      if (hist[static_cast<std::size_t>(k)].count > hist[static_cast<std::size_t>(best)].count) {
        best = k;
      }
    }
    peaks.push_back(best);
  }
  return peaks;
}

}  // namespace

SpectrumMorse::SpectrumMorse() {
  reset();
}

void SpectrumMorse::reset() {
  hist_.clear();
  next_ = 0;
  done_.clear();
  text_.clear();
  sealed_.clear();
  now_sec_ = 0.0;
  have_state_ = false;
  is_on_ = false;
  saw_mark_ = false;
  state_start_sec_ = 0.0;
  unit_seconds_ = 0.24;
  unit_calibrated_ = false;
  columns_ = 0;
  column_sec_ = 1024.0 / 48000.0;
  latched_on_ = false;
}

double SpectrumMorse::lagSeconds() const {
  /* Six dahs (18 dit-times). Until the speed is known, wait at 5 WPM. */
  const double dit = unit_calibrated_ ? std::max(0.040, unit_seconds_) : 0.24;
  return 18.0 * dit;
}

double SpectrumMorse::wpm() const {
  if (!unit_calibrated_ || unit_seconds_ <= 0.001) {
    return 0.0;
  }
  return 1.2 / unit_seconds_;
}

void SpectrumMorse::feed(float power_db, float noise_floor_db, float bin_width_hz) {
  if (!(bin_width_hz > 1.0f) || !std::isfinite(power_db)) {
    return;
  }
  const double period = columnPeriodSec(bin_width_hz);
  column_sec_ = period;
  if (columns_ > 0) {
    now_sec_ += period;
  }
  ++columns_;

  Column col;
  col.t_sec = now_sec_;
  col.on = spectrumTraceOn(power_db, noise_floor_db, latched_on_);
  latched_on_ = col.on;
  hist_.push_back(col);

  const double keep_before = now_sec_ - 60.0;
  while (hist_.size() > 2 && next_ > 0 && hist_.front().t_sec < keep_before) {
    hist_.erase(hist_.begin());
    --next_;
  }

  const double cursor = now_sec_ - lagSeconds();
  while (next_ < static_cast<int>(hist_.size()) &&
         hist_[static_cast<std::size_t>(next_)].t_sec <= cursor) {
    const Column &sample = hist_[static_cast<std::size_t>(next_)];
    if (!have_state_) {
      have_state_ = true;
      is_on_ = sample.on;
      state_start_sec_ = sample.t_sec;
      saw_mark_ = sample.on;
    } else if (sample.on != is_on_) {
      const double dur = sample.t_sec - state_start_sec_;
      if (is_on_) {
        if (dur >= 0.015) {
          pushRun(true, dur);
          saw_mark_ = true;
        }
      } else if (saw_mark_ && dur >= 0.015) {
        pushRun(false, dur);
      }
      is_on_ = sample.on;
      state_start_sec_ = sample.t_sec;
    }
    ++next_;
  }
  publish(cursor);
}

void SpectrumMorse::pushRun(bool on, double seconds) {
  Run run;
  run.on = on;
  run.sec = seconds;
  done_.push_back(run);
}

void SpectrumMorse::sealOldest() {
  if (done_.size() <= 4000) {
    return;
  }
  const Timing timing = measure(done_);
  if (!timing.ok) {
    return;
  }
  int cut = -1;
  const int limit = static_cast<int>(done_.size()) / 2;
  for (int i = 800; i < limit; ++i) {
    if (!done_[static_cast<std::size_t>(i)].on &&
        done_[static_cast<std::size_t>(i)].sec >= timing.letter_cut) {
      cut = i;
    }
  }
  if (cut < 0) {
    return;
  }
  const std::vector<Run> old(done_.begin(), done_.begin() + cut + 1);
  done_.erase(done_.begin(), done_.begin() + cut + 1);
  sealed_ += decodeRuns(old, timing);
}

SpectrumMorse::Timing SpectrumMorse::measure(const std::vector<Run> &runs) const {
  Timing timing;
  timing.dit = 0.0;
  timing.dah_cut = 0.0;
  timing.letter_cut = 0.0;
  timing.word_cut = 0.0;
  timing.ok = false;

  const double quantum = (column_sec_ > 0.005) ? column_sec_ : (1024.0 / 48000.0);
  std::vector<double> marks;
  std::vector<double> gaps;
  std::vector<LengthBin> mark_hist;
  std::vector<LengthBin> gap_hist;
  for (const Run &run : runs) {
    if (run.sec < 0.015 || run.sec >= 2.5) {
      continue;
    }
    const int bin = lengthBin(run.sec, quantum);
    if (run.on) {
      marks.push_back(run.sec);
      noteBin(mark_hist, bin, run.sec);
    } else {
      gaps.push_back(run.sec);
      noteBin(gap_hist, bin, run.sec);
    }
  }
  if (marks.size() < 3) {
    return timing;
  }

  const std::vector<int> mark_peaks = histogramPeaks(mark_hist, static_cast<int>(marks.size()));
  int dit_bin = -1;
  int dah_bin = -1;
  double best_score = 0.0;
  for (int lo : mark_peaks) {
    for (int hi : mark_peaks) {
      if (hi <= lo) {
        continue;
      }
      const double ratio = static_cast<double>(hi) / static_cast<double>(lo);
      if (ratio < 1.95 || ratio > 5.0) {
        continue;
      }
      const double score = std::fabs(std::log(ratio / 3.0));
      if (dit_bin < 0 || score < best_score) {
        best_score = score;
        dit_bin = lo;
        dah_bin = hi;
      }
    }
  }

  double dit = 0.0;
  double dah_cut = 0.0;
  if (dit_bin > 0 && dah_bin > dit_bin) {
    const double valley = 0.5 * (dit_bin + dah_bin);
    std::vector<double> dit_vals;
    std::vector<double> dah_vals;
    for (double sec : marks) {
      const int bin = lengthBin(sec, quantum);
      if (static_cast<double>(bin) < valley) {
        dit_vals.push_back(sec);
      } else if (static_cast<double>(bin) > valley) {
        dah_vals.push_back(sec);
      }
    }
    dit = dit_vals.empty() ? dit_bin * quantum : medianOf(dit_vals);
    if (!dit_vals.empty() && !dah_vals.empty()) {
      const double dit_hi = *std::max_element(dit_vals.cbegin(), dit_vals.cend());
      const double dah_lo = *std::min_element(dah_vals.cbegin(), dah_vals.cend());
      dah_cut = 0.5 * (dit_hi + dah_lo);
    } else {
      dah_cut = valley * quantum;
    }
  } else {
    dit = medianOf(marks);
    dah_cut = dit * 2.0;
  }
  if (dit < 0.020 || dit > 1.2) {
    return timing;
  }

  const std::vector<int> gap_peaks = histogramPeaks(gap_hist, static_cast<int>(gaps.size()));
  int element_bin = -1;
  int letter_bin = -1;
  int word_bin = -1;
  for (int bin : gap_peaks) {
    const double units = (bin * quantum) / dit;
    if (element_bin < 0 && units >= 0.22 && units <= 1.8) {
      element_bin = bin;
      continue;
    }
    if (letter_bin < 0 && (element_bin < 0 || bin > element_bin) &&
        units >= 1.45 && units <= 4.6) {
      letter_bin = bin;
      continue;
    }
    if (word_bin < 0 && (letter_bin < 0 || bin > letter_bin) &&
        units >= 3.8 && units <= 14.0) {
      word_bin = bin;
      continue;
    }
  }

  auto edge_cut = [&](int lo_bin, int hi_bin, double fallback) {
    if (lo_bin < 0 || hi_bin < 0) {
      return fallback;
    }
    const double mid = 0.5 * (lo_bin + hi_bin);
    double lo_max = -1.0;
    double hi_min = 1.0e9;
    for (int bin = 0; bin < static_cast<int>(gap_hist.size()); ++bin) {
      if (gap_hist[static_cast<std::size_t>(bin)].count <= 0) {
        continue;
      }
      if (bin <= mid) {
        lo_max = std::max(lo_max, gap_hist[static_cast<std::size_t>(bin)].hi);
      } else {
        hi_min = std::min(hi_min, gap_hist[static_cast<std::size_t>(bin)].lo);
      }
    }
    if (lo_max > 0.0 && hi_min < 1.0e8) {
      return 0.5 * (lo_max + hi_min);
    }
    return mid * quantum;
  };

  double letter_cut = edge_cut(element_bin, letter_bin, dit * 2.0);
  double word_cut = edge_cut(letter_bin >= 0 ? letter_bin : element_bin, word_bin,
                             std::max(letter_cut * 2.2, dit * 5.0));
  if (word_cut <= letter_cut) {
    word_cut = letter_cut * 2.2;
  }

  timing.dit = dit;
  timing.dah_cut = dah_cut;
  timing.letter_cut = letter_cut;
  timing.word_cut = word_cut;
  timing.ok = true;
  return timing;
}

std::string SpectrumMorse::decodeRuns(const std::vector<Run> &runs, const Timing &timing) const {
  std::string text;
  std::vector<double> marks;
  for (const Run &run : runs) {
    if (run.on) {
      if (run.sec >= 0.015) {
        marks.push_back(run.sec);
      }
      continue;
    }
    if (marks.empty() || run.sec < timing.letter_cut) {
      continue;
    }
    std::string pattern;
    for (double d : marks) {
      pattern.push_back(d >= timing.dah_cut ? '-' : '.');
    }
    const char ch = morseChar(pattern);
    text.push_back(ch != '\0' ? ch : '?');
    marks.clear();
    if (run.sec >= timing.word_cut) {
      text.push_back(' ');
    }
  }
  return text;
}

void SpectrumMorse::publish(double cursor_sec) {
  sealOldest();
  const Timing timing = measure(done_);
  if (!timing.ok) {
    return;
  }
  if (!unit_calibrated_) {
    unit_seconds_ = timing.dit;
    unit_calibrated_ = true;
  } else {
    unit_seconds_ = 0.85 * unit_seconds_ + 0.15 * timing.dit;
  }

  std::vector<Run> view = done_;
  if (have_state_ && !is_on_ && saw_mark_) {
    const double gap = cursor_sec - state_start_sec_;
    if (gap >= 0.015) {
      Run tail;
      tail.on = false;
      tail.sec = gap;
      view.push_back(tail);
    }
  }
  setText(sealed_ + decodeRuns(view, timing));
}

void SpectrumMorse::setText(const std::string &text) {
  if (text == text_) {
    return;
  }
  text_ = text;
  if (text_.size() > 8000) {
    const std::size_t drop = text_.size() - 6000;
    text_.erase(0, drop);
    if (sealed_.size() >= drop) {
      sealed_.erase(0, drop);
    } else {
      sealed_.clear();
    }
  }
}

#ifdef CWX_SPECTRUM_TEST
#include <cstdio>
#include <cstring>

static void feedSeconds(SpectrumMorse &dec, bool on, double seconds, float bin_width) {
  const double period = 1.0 / static_cast<double>(bin_width);
  int n = static_cast<int>(std::lround(seconds / period));
  if (n < 1) {
    n = 1;
  }
  const float power = on ? -20.0f : -90.0f;
  for (int i = 0; i < n; ++i) {
    dec.feed(power, -90.0f, bin_width);
  }
}

static void sendChar(SpectrumMorse &dec, const char *pattern, float bin_width, double dit) {
  for (const char *p = pattern; *p; ++p) {
    feedSeconds(dec, true, (*p == '-') ? 3.0 * dit : dit, bin_width);
    feedSeconds(dec, false, dit, bin_width);
  }
  /* Element space is already one dit. Two more dits finish the letter space. */
  feedSeconds(dec, false, 2.0 * dit, bin_width);
}

static void sendWordGap(SpectrumMorse &dec, float bin_width, double dit) {
  /* Letter space already added two dits after the last element dit.
   * A word space is seven dits total, so four more dits remain. */
  feedSeconds(dec, false, 4.0 * dit, bin_width);
}

int main(void) {
  const float bin_width = 48000.0f / 1024.0f;
  const double dit = 1.2 / 20.0;
  SpectrumMorse dec;
  sendChar(dec, "-.-.", bin_width, dit);
  sendChar(dec, "--.-", bin_width, dit);
  sendWordGap(dec, bin_width, dit);
  sendChar(dec, "-..", bin_width, dit);
  sendChar(dec, ".", bin_width, dit);
  sendWordGap(dec, bin_width, dit);
  /* Cover the initial six-dah wait, which starts at a 5 WPM dit. */
  feedSeconds(dec, false, 8.0, bin_width);
  const std::string got = dec.text();
  std::printf("decoded='%s' wpm=%.1f cal=%d\n", got.c_str(), dec.wpm(), dec.calibrated() ? 1 : 0);
  if (got.find("CQ DE") == std::string::npos) {
    std::fprintf(stderr, "expected CQ DE in the decode\n");
    return 1;
  }
  return 0;
}
#endif
