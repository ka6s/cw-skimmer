/**
 * @file spectrummorsewindow.cpp
 * @brief Decode CW from the spectrum picture, six dahs behind the leading edge
 *
 * A waterfall column is key-down when the tuned bin is a lit trace and
 * key-up when it is black or in the near-black foot of the gray ramp.
 * Dit, dah, letter, and word are the groups those runs form. A character
 * is copied only after six dahs of newer spectrum have been drawn.
 */

#include "spectrummorsewindow.h"
#include "cwcopyformat.h"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

double medianOf(QVector<double> v)
{
    if (v.isEmpty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const int n = v.size();
    if (n % 2 == 1) {
        return v[n / 2];
    }
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

QChar morseChar(const QString &pattern)
{
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
        if (pattern == QLatin1String(e.pat)) {
            return QLatin1Char(e.ch);
        }
    }
    return QChar();
}

/**
 * Same ramp as SpectrumWidget::powerToColor. The visible white trace sits
 * at noise+16 dB. Once the key is down it stays down until noise+12.5 dB,
 * so a one-column dip just under the white level does not split a mark.
 * A real element space falls to within a few dB of the noise floor.
 */
constexpr float kTraceAttackDb = 16.0f;
constexpr float kTraceReleaseDb = 12.5f;

bool spectrumTraceOn(float powerDb, float noiseFloorDb, bool latchedOn)
{
    if (!std::isfinite(powerDb)) {
        return false;
    }
    const float floorDb = std::max(-110.0f, noiseFloorDb);
    const float need = latchedOn ? kTraceReleaseDb : kTraceAttackDb;
    return powerDb >= floorDb + need;
}

double columnPeriodSec(float binWidthHz)
{
    if (binWidthHz > 20.0f) {
        return 1.0 / static_cast<double>(binWidthHz);
    }
    return 0.0107;
}

struct LengthBin {
    int count;
    double lo;
    double hi;
    LengthBin() : count(0), lo(0.0), hi(0.0) {}
};

int lengthBin(double sec, double quantum)
{
    const int b = static_cast<int>(std::lround(sec / quantum));
    return b < 1 ? 1 : b;
}

void noteBin(QVector<LengthBin> &hist, int bin, double sec)
{
    if (bin >= hist.size()) {
        hist.resize(bin + 1);
    }
    LengthBin &slot = hist[bin];
    if (slot.count == 0) {
        slot.lo = sec;
        slot.hi = sec;
    } else {
        slot.lo = std::min(slot.lo, sec);
        slot.hi = std::max(slot.hi, sec);
    }
    ++slot.count;
}

QVector<int> histogramPeaks(const QVector<LengthBin> &hist, int total)
{
    QVector<int> peaks;
    if (total <= 0 || hist.isEmpty()) {
        return peaks;
    }
    const int thresh = std::max(3, static_cast<int>(std::lround(0.05 * total)));
    int first = -1;
    int last = -1;
    for (int i = 0; i < hist.size(); ++i) {
        if (hist[i].count > 0) {
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
        const int count = hist[k].count;
        if (count < thresh) {
            continue;
        }
        const int left = (k > 0) ? hist[k - 1].count : 0;
        const int right = (k + 1 < hist.size()) ? hist[k + 1].count : 0;
        if (count >= left && count > right) {
            peaks.append(k);
        }
    }
    if (peaks.isEmpty()) {
        int best = first;
        for (int k = first; k <= last; ++k) {
            if (hist[k].count > hist[best].count) {
                best = k;
            }
        }
        peaks.append(best);
    }
    return peaks;
}

}  // namespace

SpectrumMorseWindow::SpectrumMorseWindow(QWidget *parent, bool headless)
    : QWidget(parent)
    , m_headless(headless)
    , m_statusLabel(nullptr)
    , m_textView(nullptr)
    , m_next(0)
    , m_nowSec(0.0)
    , m_haveState(false)
    , m_isOn(false)
    , m_sawMark(false)
    , m_stateStartSec(0.0)
    , m_unitSeconds(0.24)
    , m_unitCalibrated(false)
    , m_columns(0)
    , m_columnSec(1024.0 / 48000.0)
    , m_latchedOn(false)
{
    if (!m_headless) {
        buildUi();
    }
}

void SpectrumMorseWindow::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(
        "color: #d0d0d0; font-family: 'JetBrains Mono', monospace; font-size: 11px;");
    layout->addWidget(m_statusLabel);

    m_textView = new QPlainTextEdit(this);
    m_textView->setReadOnly(true);
    m_textView->setStyleSheet(
        "QPlainTextEdit { background: #101010; color: #f2f2f2; "
        "font-family: 'JetBrains Mono', monospace; font-size: 16px; }");
    installCwCopyHighlighter(m_textView);
    layout->addWidget(m_textView, 1);

    auto *clearButton = new QPushButton(QStringLiteral("Clear"), this);
    layout->addWidget(clearButton);
    connect(clearButton, &QPushButton::clicked, this, [this]() { clearDecode(); });

    refreshStatus();
}

void SpectrumMorseWindow::setTargetLabel(const QString &label)
{
    m_target = label;
    refreshStatus();
}

void SpectrumMorseWindow::clearDecode()
{
    m_text.clear();
    m_sealed.clear();
    m_done.clear();
    m_sawMark = false;
    m_haveState = false;
    m_isOn = false;
    m_latchedOn = false;
    if (m_textView) {
        m_textView->clear();
    }
    refreshStatus();
}

void SpectrumMorseWindow::resetTiming()
{
    m_hist.clear();
    m_next = 0;
    m_done.clear();
    m_sealed.clear();
    m_nowSec = 0.0;
    m_haveState = false;
    m_isOn = false;
    m_sawMark = false;
    m_stateStartSec = 0.0;
    m_unitSeconds = 0.24;
    m_unitCalibrated = false;
    m_columns = 0;
    m_columnSec = 1024.0 / 48000.0;
    m_latchedOn = false;
    refreshStatus();
}

QString SpectrumMorseWindow::trailingText(int maxChars) const
{
    if (maxChars <= 0 || m_text.size() <= maxChars) {
        return m_text;
    }
    return m_text.right(maxChars);
}

double SpectrumMorseWindow::lagSec() const
{
    /* Six dahs (18 dit-times). Until the speed is known, wait at 5 WPM so a
     * slow character is complete on the waterfall before the first copy. */
    const double dit = m_unitCalibrated ? std::max(0.040, m_unitSeconds) : 0.24;
    return 18.0 * dit;
}

void SpectrumMorseWindow::feedColumn(float powerDb, float noiseFloorDb, float binWidthHz)
{
    if (!(binWidthHz > 1.0f) || !std::isfinite(powerDb)) {
        return;
    }
    const double period = columnPeriodSec(binWidthHz);
    m_columnSec = period;
    if (m_columns > 0) {
        m_nowSec += period;
    }
    ++m_columns;

    Column col;
    col.tSec = m_nowSec;
    col.on = spectrumTraceOn(powerDb, noiseFloorDb, m_latchedOn);
    m_latchedOn = col.on;
    m_hist.append(col);

    const double keepBefore = m_nowSec - 60.0;
    while (m_hist.size() > 2 && m_next > 0 && m_hist.first().tSec < keepBefore) {
        m_hist.removeFirst();
        --m_next;
    }

    const double cursor = m_nowSec - lagSec();
    while (m_next < m_hist.size() && m_hist[m_next].tSec <= cursor) {
        const Column &sample = m_hist[m_next];
        if (!m_haveState) {
            m_haveState = true;
            m_isOn = sample.on;
            m_stateStartSec = sample.tSec;
            m_sawMark = sample.on;
        } else if (sample.on != m_isOn) {
            const double dur = sample.tSec - m_stateStartSec;
            if (m_isOn) {
                if (dur >= 0.015) {
                    pushRun(true, dur);
                    m_sawMark = true;
                }
            } else if (m_sawMark && dur >= 0.015) {
                pushRun(false, dur);
            }
            m_isOn = sample.on;
            m_stateStartSec = sample.tSec;
        }
        ++m_next;
    }
    publish(cursor);

    if (!m_headless && (m_columns % 8) == 0) {
        refreshStatus();
    }
}

void SpectrumMorseWindow::pushRun(bool on, double seconds)
{
    Run run;
    run.on = on;
    run.sec = seconds;
    m_done.append(run);
}

void SpectrumMorseWindow::sealOldest()
{
    if (m_done.size() <= 4000) {
        return;
    }
    const Timing timing = measure(m_done);
    if (!timing.ok) {
        return;
    }
    int cut = -1;
    const int limit = m_done.size() / 2;
    for (int i = 800; i < limit; ++i) {
        if (!m_done[i].on && m_done[i].sec >= timing.letterCut) {
            cut = i;
        }
    }
    if (cut < 0) {
        return;
    }
    const QVector<Run> old = m_done.mid(0, cut + 1);
    m_done = m_done.mid(cut + 1);
    m_sealed += decodeRuns(old, timing);
}

SpectrumMorseWindow::Timing SpectrumMorseWindow::measure(const QVector<Run> &runs) const
{
    Timing timing;
    timing.dit = 0.0;
    timing.dahCut = 0.0;
    timing.letterCut = 0.0;
    timing.wordCut = 0.0;
    timing.ok = false;

    const double quantum = (m_columnSec > 0.005) ? m_columnSec : (1024.0 / 48000.0);
    QVector<double> marks;
    QVector<double> gaps;
    QVector<LengthBin> markHist;
    QVector<LengthBin> gapHist;
    for (const Run &run : runs) {
        if (run.sec < 0.015 || run.sec >= 2.5) {
            continue;
        }
        const int bin = lengthBin(run.sec, quantum);
        if (run.on) {
            marks.append(run.sec);
            noteBin(markHist, bin, run.sec);
        } else {
            gaps.append(run.sec);
            noteBin(gapHist, bin, run.sec);
        }
    }
    if (marks.size() < 3) {
        return timing;
    }

    /* Dit and dah are the two histogram peaks near a 1:3 ratio. A handful of
     * odd lengths must not bridge that valley the way a sorted-adjacent split did. */
    const QVector<int> markPeaks = histogramPeaks(markHist, marks.size());
    int ditBin = -1;
    int dahBin = -1;
    double bestScore = 0.0;
    for (int lo : markPeaks) {
        for (int hi : markPeaks) {
            if (hi <= lo) {
                continue;
            }
            const double ratio = static_cast<double>(hi) / static_cast<double>(lo);
            if (ratio < 1.95 || ratio > 5.0) {
                continue;
            }
            const double score = std::fabs(std::log(ratio / 3.0));
            if (ditBin < 0 || score < bestScore) {
                bestScore = score;
                ditBin = lo;
                dahBin = hi;
            }
        }
    }

    double dit = 0.0;
    double dahCut = 0.0;
    if (ditBin > 0 && dahBin > ditBin) {
        const double valley = 0.5 * (ditBin + dahBin);
        QVector<double> ditVals;
        QVector<double> dahVals;
        for (double sec : marks) {
            const int bin = lengthBin(sec, quantum);
            if (static_cast<double>(bin) < valley) {
                ditVals.append(sec);
            } else if (static_cast<double>(bin) > valley) {
                dahVals.append(sec);
            }
        }
        dit = ditVals.isEmpty() ? ditBin * quantum : medianOf(ditVals);
        if (!ditVals.isEmpty() && !dahVals.isEmpty()) {
            const double ditHi = *std::max_element(ditVals.cbegin(), ditVals.cend());
            const double dahLo = *std::min_element(dahVals.cbegin(), dahVals.cend());
            dahCut = 0.5 * (ditHi + dahLo);
        } else {
            dahCut = valley * quantum;
        }
    } else {
        dit = medianOf(marks);
        dahCut = dit * 2.0;
    }
    if (dit < 0.020 || dit > 1.2) {
        return timing;
    }

    /* Element, letter, and word spaces are the short, middle, and long gap
     * peaks, in that order. Smear makes a fast element space much shorter
     * than the measured dit, so the match is wide. */
    const QVector<int> gapPeaks = histogramPeaks(gapHist, gaps.size());
    int elementBin = -1;
    int letterBin = -1;
    int wordBin = -1;
    for (int bin : gapPeaks) {
        const double units = (bin * quantum) / dit;
        if (elementBin < 0 && units >= 0.22 && units <= 1.8) {
            elementBin = bin;
            continue;
        }
        if (letterBin < 0 && (elementBin < 0 || bin > elementBin) &&
            units >= 1.45 && units <= 4.6) {
            letterBin = bin;
            continue;
        }
        if (wordBin < 0 && (letterBin < 0 || bin > letterBin) &&
            units >= 3.8 && units <= 14.0) {
            wordBin = bin;
            continue;
        }
    }

    auto edgeCut = [&](int loBin, int hiBin, double fallback) {
        if (loBin < 0 || hiBin < 0) {
            return fallback;
        }
        const double mid = 0.5 * (loBin + hiBin);
        double loMax = -1.0;
        double hiMin = 1.0e9;
        for (int bin = 0; bin < gapHist.size(); ++bin) {
            if (gapHist[bin].count <= 0) {
                continue;
            }
            if (bin <= mid) {
                loMax = std::max(loMax, gapHist[bin].hi);
            } else {
                hiMin = std::min(hiMin, gapHist[bin].lo);
            }
        }
        if (loMax > 0.0 && hiMin < 1.0e8) {
            return 0.5 * (loMax + hiMin);
        }
        return mid * quantum;
    };

    double letterCut = edgeCut(elementBin, letterBin, dit * 2.0);
    double wordCut = edgeCut(letterBin >= 0 ? letterBin : elementBin, wordBin,
                             std::max(letterCut * 2.2, dit * 5.0));
    if (wordCut <= letterCut) {
        wordCut = letterCut * 2.2;
    }

    timing.dit = dit;
    timing.dahCut = dahCut;
    timing.letterCut = letterCut;
    timing.wordCut = wordCut;
    timing.ok = true;
    return timing;
}

QString SpectrumMorseWindow::decodeRuns(const QVector<Run> &runs, const Timing &timing) const
{
    QString text;
    QVector<double> marks;
    for (const Run &run : runs) {
        if (run.on) {
            if (run.sec >= 0.015) {
                marks.append(run.sec);
            }
            continue;
        }
        if (marks.isEmpty() || run.sec < timing.letterCut) {
            continue;
        }
        QString pattern;
        for (double d : marks) {
            pattern.append(d >= timing.dahCut ? QChar('-') : QChar('.'));
        }
        const QChar ch = morseChar(pattern);
        text.append(ch.isNull() ? QChar('?') : ch);
        marks.clear();
        if (run.sec >= timing.wordCut) {
            text.append(QChar(' '));
        }
    }
    return text;
}

void SpectrumMorseWindow::publish(double cursorSec)
{
    sealOldest();
    const Timing timing = measure(m_done);
    if (!timing.ok) {
        return;
    }
    if (!m_unitCalibrated) {
        m_unitSeconds = timing.dit;
        m_unitCalibrated = true;
    } else {
        m_unitSeconds = 0.85 * m_unitSeconds + 0.15 * timing.dit;
    }

    QVector<Run> view = m_done;
    if (m_haveState && !m_isOn && m_sawMark) {
        const double gap = cursorSec - m_stateStartSec;
        if (gap >= 0.015) {
            Run tail;
            tail.on = false;
            tail.sec = gap;
            view.append(tail);
        }
    }
    setText(m_sealed + decodeRuns(view, timing));
}

void SpectrumMorseWindow::setText(const QString &text)
{
    if (text == m_text) {
        return;
    }
    m_text = text;
    if (m_text.size() > 8000) {
        const int drop = m_text.size() - 6000;
        m_text.remove(0, drop);
        if (m_sealed.size() >= drop) {
            m_sealed.remove(0, drop);
        } else {
            m_sealed.clear();
        }
    }
    if (m_textView) {
        m_textView->setPlainText(m_text);
        applyCwCopyColors(m_textView);
    }
}

void SpectrumMorseWindow::refreshStatus()
{
    if (!m_statusLabel) {
        return;
    }
    const double wpm = (m_unitSeconds > 0.001) ? (1.2 / m_unitSeconds) : 0.0;
    QString line = QStringLiteral(
                       "Spectrum scope  ·  white = on, black = off  ·  "
                       "copy waits %1 s (6 dahs)  ·  %2 WPM")
                       .arg(lagSec(), 0, 'f', 2)
                       .arg(m_unitCalibrated ? wpm : 0.0, 0, 'f', 0);
    if (!m_target.isEmpty()) {
        line += QStringLiteral("  ·  ") + m_target;
    }
    m_statusLabel->setText(line);
}
