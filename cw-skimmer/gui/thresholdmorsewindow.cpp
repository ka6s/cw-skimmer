/**
 * @file thresholdmorsewindow.cpp
 * @brief Append-only scrolling Morse decoder with bimodal WPM estimate
 *
 * Validated on captures:
 *   135243 (expected "49"): dits ~200 ms, dahs ~600 ms → unit ≈ 200 ms (~6 WPM)
 *   141627 (expected "RPL"): unit ~200 ms + moderate Schmitt hyst
 *   144653 (ear "TUNC7"): scope envelope yields NC7 at unit ~220 ms;
 *     short noise must not pull unit to ~100 ms (T/E spam).
 */

#include "thresholdmorsewindow.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QTextCursor>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>

namespace {

const std::map<QString, char> &morseTable()
{
    static const std::map<QString, char> table = {
        {".-", 'A'},   {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
        {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
        {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
        {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
        {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-.-.--", '!'},
        {"-....-", '-'}, {"-..-.", '/'},  {".-.-.", '+'},  {"-...-", '='},
        {"...-.-", '*'},
    };
    return table;
}

double ditSecondsForWpm(int wpm)
{
    /* 1 WPM → 1.2 s dit (QRS); 40 WPM → 30 ms */
    const int w = std::max(1, std::min(60, wpm));
    return 1.2 / static_cast<double>(w);
}

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

}  // namespace

ThresholdMorseWindow::ThresholdMorseWindow(QWidget *parent, bool headless)
    : QWidget(parent)
    , m_headless(headless)
    , m_statusLabel(nullptr)
    , m_elementLabel(nullptr)
    , m_timingLabel(nullptr)
    , m_wpmLabel(nullptr)
    , m_textView(nullptr)
    , m_clearButton(nullptr)
    , m_resetTimingButton(nullptr)
    , m_autoWpmCheck(nullptr)
    , m_wpmSlider(nullptr)
    , m_hasState(false)
    , m_isHigh(false)
    , m_stateStartMs(0)
    , m_lastSampleMs(0)
    , m_thresholdDb(-80.0f)
    , m_unitSeconds(ditSecondsForWpm(12))
    , m_sliderUnitSeconds(ditSecondsForWpm(12))
    , m_pendingMarkSec(0.0)
    , m_pendingMarkPeakDb(-200.0f)
    , m_havePendingMark(false)
    , m_letterEmittedForSpace(false)
    , m_unitCalibrated(false)
    , m_inValley(false)
    , m_valleyStartMs(-1)
    , m_segPeakDb(-200.0f)
    , m_lastPowerDb(-200.0f)
    , m_sampleCount(0)
    , m_markCount(0)
    , m_spaceCount(0)
    , m_squelchOpen(false)
    , m_playbackMode(false)
    , m_markEnergySec(0.0)
    , m_lastStrongMarkMs(0)
    , m_lastMarkEndMs(0)
    , m_peakAboveThreshDb(-100.0f)
    , m_spamHintCount(0)
{
    if (m_headless) {
        /* Multi-channel: no widgets; Auto WPM always on. */
        hide();
        m_unitSeconds = ditSecondsForWpm(12);
        m_sliderUnitSeconds = m_unitSeconds;
        m_clock.start();
        return;
    }

    /* Diagnostic status / element / timing lines removed — they cluttered
     * the UI above the Code speed slider. WPM still shown on the slider row. */
    m_statusLabel = nullptr;
    m_elementLabel = nullptr;
    m_timingLabel = nullptr;

    m_wpmSlider = new QSlider(Qt::Horizontal, this);
    m_wpmSlider->setRange(1, 40);  /* 1 WPM ≈ 1.2 s dit (very slow CQ); 3 WPM ≈ 400 ms */
    m_wpmSlider->setValue(12);
    m_wpmSlider->setTickInterval(5);
    m_wpmSlider->setTickPosition(QSlider::TicksBelow);
    m_wpmSlider->setToolTip(
        "Manual WPM when Auto is off.\n"
        "Auto clusters mark lengths (dit vs dah) and only classifies a letter "
        "when it is committed — past text is never rewritten.");

    /* Dark lettering so WPM is readable on the dialog background */
    m_wpmLabel = new QLabel("12 WPM", this);
    m_wpmLabel->setMinimumWidth(120);
    m_wpmLabel->setAlignment(Qt::AlignCenter);
    m_wpmLabel->setToolTip(
        "Effective code speed used for decoding.\n"
        "Auto WPM ON: estimated from recent marks.\n"
        "Auto WPM OFF: value from the slider.");
    m_wpmLabel->setMinimumHeight(44);
    m_wpmLabel->setStyleSheet(
        "QLabel { color: #00008b; background: #f0f0f0; border: 1px solid darkblue; "
        "border-radius: 6px; padding: 4px 10px; "
        "font-family: 'JetBrains Mono', Tahoma, monospace; "
        "font-size: 15px; font-weight: bold; }");

    auto *wpmRow = new QHBoxLayout();
    wpmRow->addWidget(new QLabel("Code speed:", this));
    wpmRow->addWidget(m_wpmSlider, 1);
    wpmRow->addWidget(m_wpmLabel);

    m_textView = new QPlainTextEdit(this);
    m_textView->setReadOnly(true);
    m_textView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_textView->setPlaceholderText(
        "Decoded text scrolls here (append only).\n"
        "Squelched until strong CW keying is heard.\n"
        "Long runs of T or E usually mean wrong WPM or noise.");
    m_textView->setStyleSheet(
        "QPlainTextEdit { background: #0a0a0c; color: #00ff66; font-family: Courier; "
        "font-size: 18px; }");

    m_clearButton = new QPushButton("Clear Text", this);
    m_resetTimingButton = new QPushButton("Reset Timing", this);
    m_autoWpmCheck = new QCheckBox("Auto WPM", this);
    m_autoWpmCheck->setChecked(true);
    m_autoWpmCheck->setToolTip(
        "Estimate dit length by clustering recent mark durations (dit vs dah).\n"
        "Letters are classified at commit time with that unit.\n"
        "Already printed text is never changed.");

    auto *row = new QHBoxLayout();
    row->addWidget(m_autoWpmCheck);
    row->addStretch(1);
    row->addWidget(m_resetTimingButton);
    row->addWidget(m_clearButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(wpmRow);
    layout->addWidget(m_textView, 1);
    layout->addLayout(row);
    setLayout(layout);

    connect(m_clearButton, &QPushButton::clicked, this, &ThresholdMorseWindow::onClearClicked);
    connect(m_resetTimingButton, &QPushButton::clicked, this,
            &ThresholdMorseWindow::onResetTimingClicked);
    connect(m_wpmSlider, &QSlider::valueChanged, this, &ThresholdMorseWindow::onWpmSliderChanged);
    connect(m_autoWpmCheck, &QCheckBox::toggled, this, &ThresholdMorseWindow::onAutoToggled);

    applyWpmFromSlider();
    m_clock.start();
    updateUi();
}

int ThresholdMorseWindow::wpm() const
{
    if (m_headless) {
        return (m_unitSeconds > 0.001) ? static_cast<int>(std::lround(1.2 / m_unitSeconds)) : 12;
    }
    return m_wpmSlider ? m_wpmSlider->value() : 12;
}

QString ThresholdMorseWindow::trailingText(int maxChars) const
{
    if (maxChars <= 0 || m_scrollText.isEmpty()) {
        return QString();
    }
    if (m_scrollText.size() <= maxChars) {
        return m_scrollText;
    }
    return m_scrollText.right(maxChars);
}

void ThresholdMorseWindow::applyWpmFromSlider()
{
    if (m_headless) {
        return;
    }
    m_sliderUnitSeconds = ditSecondsForWpm(wpm());
    if (!m_autoWpmCheck || !m_autoWpmCheck->isChecked()) {
        m_unitSeconds = m_sliderUnitSeconds;
        m_unitCalibrated = true;
    }
}

void ThresholdMorseWindow::setTargetLabel(const QString &label)
{
    m_targetLabel = label;
    updateUi();
}

void ThresholdMorseWindow::setThresholdDb(float thresholdDb)
{
    m_thresholdDb = thresholdDb;
    updateUi();
}

void ThresholdMorseWindow::feedSample(float powerDb, bool aboveThreshold, qint64 sampleTimeMs)
{
    m_lastPowerDb = powerDb;
    m_sampleCount++;

    /* sampleTimeMs ≥ 0 is valid (incl. 0 at playback start). < 0 → wall clock. */
    const qint64 nowMs = (sampleTimeMs >= 0) ? sampleTimeMs : m_clock.elapsed();
    m_lastSampleMs = nowMs;

    const float aboveDb = powerDb - m_thresholdDb;
    if (aboveThreshold && aboveDb > m_peakAboveThreshDb) {
        m_peakAboveThreshDb = aboveDb;
    }
    m_peakAboveThreshDb *= 0.9995f;

    updateSquelch(powerDb, aboveThreshold, nowMs);

    if (!m_hasState) {
        m_hasState = true;
        m_isHigh = aboveThreshold;
        m_stateStartMs = nowMs;
        m_inValley = false;
        m_valleyStartMs = -1;
        m_segPeakDb = powerDb;
        if (aboveThreshold) {
            m_pendingMarkPeakDb = powerDb;
        }
        updateUi();
        return;
    }

    if (aboveThreshold != m_isHigh) {
        m_inValley = false;
        m_valleyStartMs = -1;
        processStateChange(aboveThreshold, nowMs);
        if (aboveThreshold) {
            m_segPeakDb = powerDb;
        }
    } else {
        if (m_isHigh) {
            m_pendingMarkPeakDb = std::max(m_pendingMarkPeakDb, powerDb);
            m_segPeakDb = std::max(m_segPeakDb, powerDb);
            /* Split glued multi-dah letters (O=---) when key never drops */
            processIntraMarkValley(powerDb, nowMs);
        } else if (m_squelchOpen) {
            checkLetterTimeout(nowMs);
        }
    }

    updateUi();
}

void ThresholdMorseWindow::processIntraMarkValley(float powerDb, qint64 nowMs)
{
    if (!m_isHigh) {
        m_inValley = false;
        m_valleyStartMs = -1;
        return;
    }

    const double unit = std::max(0.045, m_unitSeconds);
    /*
     * Valley = amplitude drop between dahs of O while Schmitt key stays high.
     * Require both: well below segment peak, and not a strong mark top.
     * cwtrace_20260801_155432: O valleys go to −70 while peaks ~−53 (thr ~−54).
     */
    const float peakRef = std::max(m_segPeakDb, m_pendingMarkPeakDb);
    const float dropDb = 5.0f;
    const bool deepVsPeak = (peakRef > -180.0f) && (powerDb <= peakRef - dropDb);
    const bool softVsThr = powerDb < (m_thresholdDb + 1.5f);
    const bool inValleyNow = deepVsPeak && softVsThr;

    /* Minimum mark body before we accept a split (avoid chopping dits) */
    const double minBody = m_unitCalibrated ? std::max(0.050, unit * 0.85)
                                            : 0.070;
    /* Element gap: ~0.5–1.2 dit; allow 35–150 ms */
    const double minValley = m_unitCalibrated ? std::max(0.035, unit * 0.45)
                                              : 0.040;
    const double maxValley = m_unitCalibrated ? std::min(0.22, unit * 1.6)
                                              : 0.18;

    if (inValleyNow) {
        if (!m_inValley) {
            m_inValley = true;
            m_valleyStartMs = nowMs;
        } else if (m_valleyStartMs >= 0) {
            const double vSec = (nowMs - m_valleyStartMs) / 1000.0;
            const double bodyToValley = (m_valleyStartMs - m_stateStartMs) / 1000.0;
            if (vSec >= minValley && bodyToValley >= minBody) {
                splitMarkAtValley(m_valleyStartMs, nowMs);
            } else if (vSec > maxValley && bodyToValley >= minBody * 0.7) {
                /* Long soft dip — still split (slow fists) */
                splitMarkAtValley(m_valleyStartMs, nowMs);
            }
        }
    } else {
        /* Rising out of valley: start next mark segment */
        if (m_inValley) {
            m_inValley = false;
            m_valleyStartMs = -1;
            m_stateStartMs = nowMs;
            m_segPeakDb = powerDb;
            m_pendingMarkPeakDb = powerDb;
        }
    }
}

void ThresholdMorseWindow::splitMarkAtValley(qint64 valleyStartMs, qint64 nowMs)
{
    if (valleyStartMs <= m_stateStartMs) {
        m_inValley = false;
        m_valleyStartMs = -1;
        return;
    }
    const double markSec = (valleyStartMs - m_stateStartMs) / 1000.0;
    const double valleySoFar = (nowMs - valleyStartMs) / 1000.0;

    /* Close current mark at valley onset */
    handleMarkEnd(markSec);
    /*
     * Element-class space (not letter): commit mark into pending letter,
     * do not flush the letter.
     */
    const double unit = std::max(0.045, m_unitSeconds);
    const double elemGap = std::min(std::max(valleySoFar, unit * 0.7), unit * 1.4);
    handleSpaceEnd(elemGap);

    /* Resume as still in key-high mark after the valley */
    m_isHigh = true;
    m_stateStartMs = nowMs;
    m_inValley = false;
    m_valleyStartMs = -1;
    m_segPeakDb = m_lastPowerDb;
    m_pendingMarkPeakDb = m_lastPowerDb;
    m_letterEmittedForSpace = false;
}

void ThresholdMorseWindow::updateSquelch(float powerDb, bool above, qint64 nowMs)
{
    /* Capture playback: never gate or close squelch — every mark must decode */
    if (m_playbackMode) {
        m_squelchOpen = true;
        if (above) {
            m_lastStrongMarkMs = nowMs;
        }
        return;
    }

    const float strengthDb = powerDb - m_thresholdDb;
    if (above && strengthDb >= 2.0f) {
        m_lastStrongMarkMs = nowMs;
    }

    if (!m_squelchOpen) {
        /* Open quickly: one solid mark, or modest energy + peak */
        if ((m_markEnergySec >= 0.12 && m_peakAboveThreshDb >= 2.5f)
            || (m_markCount >= 1 && m_peakAboveThreshDb >= 3.0f)
            || (above && strengthDb >= 4.0f && m_markCount >= 1)) {
            m_squelchOpen = true;
        }
    } else if (m_lastStrongMarkMs > 0 && (nowMs - m_lastStrongMarkMs) > 8000) {
        /* 8 s key-up for very slow CQ (was 2.5 s — closed mid-message at 3 WPM) */
        m_squelchOpen = false;
        m_pendingMarkDurs.clear();
        m_pendingMarkPeaks.clear();
        m_havePendingMark = false;
        m_pendingMarkSec = 0.0;
        m_pendingMarkPeakDb = -200.0f;
        m_markEnergySec *= 0.4;
    }

    m_markEnergySec *= 0.9992;
}

void ThresholdMorseWindow::processStateChange(bool nowHigh, qint64 nowMs)
{
    const double duration = std::max(0.0005, (nowMs - m_stateStartMs) / 1000.0);

    if (m_isHigh) {
        /* If we were mid-valley, end mark at valley start not now */
        double markDur = duration;
        if (m_inValley && m_valleyStartMs > m_stateStartMs) {
            markDur = std::max(0.0005, (m_valleyStartMs - m_stateStartMs) / 1000.0);
        }
        handleMarkEnd(markDur);
        const float strength = m_lastPowerDb - m_thresholdDb;
        if (strength >= 2.0f || markDur >= 0.08) {
            m_lastStrongMarkMs = nowMs;
            m_lastMarkEndMs = nowMs;
            m_markEnergySec += markDur;
        }
        m_inValley = false;
        m_valleyStartMs = -1;
    } else {
        handleSpaceEnd(duration);
    }

    m_isHigh = nowHigh;
    m_stateStartMs = nowMs;
    if (nowHigh) {
        m_letterEmittedForSpace = false;
        m_pendingMarkPeakDb = m_lastPowerDb;
        m_segPeakDb = m_lastPowerDb;
        m_inValley = false;
        m_valleyStartMs = -1;
    }
}

void ThresholdMorseWindow::handleMarkEnd(double seconds)
{
    if (m_havePendingMark) {
        m_pendingMarkSec += seconds;
        m_pendingMarkPeakDb = std::max(m_pendingMarkPeakDb, m_lastPowerDb);
    } else {
        m_pendingMarkSec = seconds;
        m_pendingMarkPeakDb = m_lastPowerDb;
        m_havePendingMark = true;
    }
}

void ThresholdMorseWindow::commitPendingMarkToLetter()
{
    if (!m_havePendingMark) {
        return;
    }
    double markSec = m_pendingMarkSec;
    const float markPeak = m_pendingMarkPeakDb;
    m_havePendingMark = false;
    m_pendingMarkSec = 0.0;
    m_pendingMarkPeakDb = -200.0f;

    /*
     * Short dits (E/T fragments) at 15–20 WPM are ~35–70 ms. Uncalibrated
     * 50 ms floor and 0.28×unit dropped the lone E in KENWOO (38 ms).
     * Keep a low absolute floor; scale gently with unit once calibrated.
     */
    const double minMark = m_unitCalibrated
                               ? std::max(0.025, m_unitSeconds * 0.20)
                               : 0.028;
    if (markSec < minMark) {
        return;
    }

    /*
     * Safety net for glued O (---): one long mark ~3× dah when valleys
     * were not detected. Split into equal dahs rather than one super-T.
     * OA capture: 1139 ms with unit~0.1 → three ~0.38 s dahs.
     */
    const double unit = std::max(0.040, m_unitSeconds);
    const double dahLen = unit * 3.0;
    if (m_unitCalibrated && markSec >= dahLen * 2.4) {
        int nDah = static_cast<int>(std::lround(markSec / dahLen));
        nDah = std::max(2, std::min(5, nDah));
        /* Prefer 3 for classic O when close to 3 dahs */
        if (markSec >= dahLen * 2.6 && markSec <= dahLen * 3.6) {
            nDah = 3;
        }
        const double each = markSec / static_cast<double>(nDah);
        if (each >= unit * 1.6) {
            for (int i = 0; i < nDah; ++i) {
                m_markCount++;
                noteMarkDuration(each);
                if (m_squelchOpen) {
                    m_pendingMarkDurs.append(each);
                    m_pendingMarkPeaks.append(markPeak);
                }
            }
            recomputeUnitFromMarks();
            if (m_squelchOpen && m_pendingMarkDurs.size() > 10) {
                flushPendingLetter();
            }
            return;
        }
    }

    m_markCount++;
    noteMarkDuration(markSec);
    recomputeUnitFromMarks();

    if (m_squelchOpen) {
        m_pendingMarkDurs.append(markSec);
        m_pendingMarkPeaks.append(markPeak);
        if (m_pendingMarkDurs.size() > 10) {
            flushPendingLetter();
        }
    }
}

void ThresholdMorseWindow::handleSpaceEnd(double seconds)
{
    /*
     * Merge only *tiny* thr dropouts inside a mark — not real element gaps.
     * 0.40×unit (~28 ms at 15 WPM) was ok; keep ≤0.35× and ≤45 ms so a short
     * E is not glued to a following dah across a real space (or vice versa).
     */
    const double holeMax = m_unitCalibrated
                               ? std::min(0.045, std::max(0.020, m_unitSeconds * 0.32))
                               : 0.040;

    if (seconds < holeMax && m_havePendingMark) {
        m_pendingMarkSec += seconds;
        return;
    }

    commitPendingMarkToLetter();
    m_spaceCount++;

    if (!m_squelchOpen) {
        m_pendingMarkDurs.clear();
        m_pendingMarkPeaks.clear();
        return;
    }

    /*
     * Letter may already have been emitted by checkLetterTimeout mid-gap.
     * Still apply word-space / letter-gap bookkeeping for the full gap length.
     */
    if (m_pendingMarkDurs.isEmpty()) {
        m_pendingMarkPeaks.clear();
        if (m_letterEmittedForSpace) {
            const double wordMin = wordMinSeconds();
            if (seconds >= wordMin) {
                appendChar(QChar(' '));
            } else {
                noteLetterGap(seconds);
            }
        }
        return;
    }

    /*
     * Element gap ≈ 1 dit, letter gap ≈ 3 dits (ideal).
     * Use 2.10× (not 2.5) so tight fists still split letters — BT capture
     * had ~445 ms letter gap with dit ~200 ms (~2.2×). 2.5× glued B+T
     * into -...- (?). Still ≥2.0 so a slightly long element gap
     * (e.g. 390 ms at unit 184) does not split a "9".
     * Before calibration: keep letter open for gaps < 800 ms so very slow CW
     * (CQ DE W5MHG, unit ~400 ms, element gaps ~400–560 ms) is not flushed
     * after the first two dahs as "M" instead of "Q" (--.-).
     */
    /* Floor unit so glitch-sized dits cannot turn every real gap into a letter (T/E spam). */
    const double unit = std::max(0.050, m_unitSeconds);
    const double elemMax = m_unitCalibrated ? (unit * 2.20) : 0.80;
    if (seconds < elemMax) {
        /* Intra-letter gap ≈ 1 dit — feed Auto WPM and re-estimate promptly */
        noteElementGap(seconds);
        recomputeUnitFromMarks();
        return;
    }

    recomputeUnitFromMarks();
    flushPendingLetter();
    m_letterEmittedForSpace = true;

    /*
     * Word gap: ITU ideal is 7 dits, but fists vary. GAFD ERON TR has letter
     * gaps ~0.6–0.9 s and word gaps ~1.1–1.5 s at unit ~180 ms (word only
     * ~6–8× unit). Fixed 8.5× glued words; fixed 5.5× split I5JJ. Track
     * recent letter-class gaps and place the word threshold just above them.
     * Very slow CQ fists often use ~3× unit for ALL character gaps (no
     * longer word pause) — protocol spacing (CQ/DE) handles those.
     */
    const double wordMin = wordMinSeconds();
    if (seconds >= wordMin) {
        appendChar(QChar(' '));
    } else {
        noteLetterGap(seconds);
    }
}

void ThresholdMorseWindow::checkLetterTimeout(qint64 nowMs)
{
    if (!m_squelchOpen || m_letterEmittedForSpace) {
        return;
    }
    /* Must not be mid-mark: timeout only while key is up */
    if (m_isHigh) {
        return;
    }

    const double hold = (nowMs - m_stateStartMs) / 1000.0;
    /*
     * Letter timeout must be ABOVE elemMax (2.10×) so element gaps never
     * trigger it. Was 2.15× — OK — but MUST commit the just-ended mark
     * first. Without that, W5MHG Q=--.- became G=--. + T=- because the
     * final dah was still in m_pendingMarkSec when timeout flushed.
     */
    const double unit = std::max(0.050, m_unitSeconds);
    const double letterMin = m_unitCalibrated ? (unit * 2.60) : 0.90;
    if (hold < letterMin) {
        return;
    }

    /* Include the mark that just ended (still in hole-merge holding pen) */
    commitPendingMarkToLetter();
    if (m_pendingMarkDurs.isEmpty()) {
        return;
    }

    recomputeUnitFromMarks();
    flushPendingLetter();
    m_letterEmittedForSpace = true;
    const double wordMin = wordMinSeconds();
    if (hold >= wordMin && !m_scrollText.isEmpty() && !m_scrollText.endsWith(QChar(' '))) {
        appendChar(QChar(' '));
    } else if (hold < wordMin) {
        noteLetterGap(hold);
    }
}

void ThresholdMorseWindow::flushPendingLetter()
{
    if (m_pendingMarkDurs.isEmpty()) {
        return;
    }

    recomputeUnitFromMarks();
    QVector<double> durs = m_pendingMarkDurs;
    QVector<float> peaks = m_pendingMarkPeaks;
    m_pendingMarkDurs.clear();
    m_pendingMarkPeaks.clear();
    while (peaks.size() < durs.size()) {
        peaks.append(-200.0f);
    }

    /*
     * Leading noise dit glued onto a letter:
     *  - 50 ms blip + --. (G) → .--. (P) "POTA" instead of "GOTA"
     *  - 60 ms blip + dah (T) → .- (A) on GAFD ERON TR end
     * Drop the prefix when it is much shorter than a real dit and the rest
     * is already a valid letter. Do not strip L's first dit (.-.. → -.. = D)
     * when it is comparable in length to the later dits.
     */
    if (durs.size() >= 2 && durs[0] < m_unitSeconds * 0.55
        && durs[0] < m_unitSeconds * 2.0) {
        bool strip = false;
        if (durs.size() >= 3) {
            QVector<double> otherDits;
            for (int i = 1; i < durs.size(); ++i) {
                if (durs[i] < m_unitSeconds * 2.0) {
                    otherDits.append(durs[i]);
                }
            }
            if (!otherDits.isEmpty()) {
                std::sort(otherDits.begin(), otherDits.end());
                const double medDit = otherDits[otherDits.size() / 2];
                if (durs[0] < medDit * 0.40) {
                    strip = true;
                }
            }
        } else if (durs[0] < m_unitSeconds * 0.45 && durs[0] < durs[1] * 0.35) {
            /* Two-element: noise dit + real mark (e.g. thr chatter before T) */
            strip = true;
        }
        if (strip) {
            QString alt;
            for (int i = 1; i < durs.size(); ++i) {
                alt.append(durs[i] >= m_unitSeconds * 2.0 ? QChar('-') : QChar('.'));
            }
            if (morseToChar(alt) != 0) {
                durs.removeFirst();
                if (!peaks.isEmpty()) {
                    peaks.removeFirst();
                }
            }
        }
    }

    /*
     * Dit/dah boundary: 2.0×unit is standard. Slightly soft 1.9× helps A (.-)
     * when the dah is a bit short (OA capture second element of A).
     */
    const double dahMin = m_unitSeconds * 1.90;
    QString pattern;
    for (double d : durs) {
        pattern.append(d >= dahMin ? QChar('-') : QChar('.'));
    }

    /*
     * Drop only *noise* single-element blips — not real short E/T.
     * Capture KENWOO: lone E was 38 ms / peak +1.3 dB while dahs were +4 dB;
     * 0.45×unit and weak-peak filters erased it. Accept any single dit ≥25 ms
     * that cleared threshold (mark already above thr in the scope path).
     */
    float letterPeak = -200.0f;
    for (float p : peaks) {
        letterPeak = std::max(letterPeak, p);
    }

    if (durs.size() == 1) {
        const double d = durs[0];
        const double minE = m_unitCalibrated ? std::max(0.025, m_unitSeconds * 0.28)
                                            : 0.025;
        /* T must look like a dah: ≥1.1× unit (was 1.2 — still ok for short T) */
        const double minT = m_unitCalibrated ? std::max(0.10, m_unitSeconds * 1.05)
                                            : 0.12;
        if (pattern == QLatin1String(".") && d < minE) {
            return;
        }
        if (pattern == QLatin1String("-") && d < minT) {
            return;
        }
    }

    const char rawCh = morseToChar(pattern);

    /*
     * Glued multi-letter run (common when letter gap < 2.1×unit):
     * e.g. .-----... is not a single Morse char, but longest-match splits
     * to .---- + -... = "1B" (cwtrace_20260731_135955: N 1B KPL …).
     * Prefer longest valid code from the left so .---- wins over .--- (J).
     */
    QString emitChars;
    if (rawCh != 0) {
        emitChars = QString(QChar(rawCh));
    } else if (pattern.size() >= 5) {
        int i = 0;
        while (i < pattern.size()) {
            bool matched = false;
            const int maxL = std::min(6, pattern.size() - i);
            for (int L = maxL; L >= 1; --L) {
                const QString sub = pattern.mid(i, L);
                const char c = morseToChar(sub);
                if (c != 0) {
                    emitChars.append(QChar(c));
                    i += L;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                emitChars.append(QChar('?'));
                ++i;
            }
        }
    } else {
        emitChars = QString(QChar('?'));
    }

    /*
     * Weak trailing noise: only drop single I/E/T that is *barely* above thr
     * AND far below the established mark median. Do not use thr+6 (that killed
     * the weak but valid E at thr−6 / peak+1.3). Require thr+2.5 or less.
     */
    if (emitChars.size() == 1 && !m_recentLetterPeaks.isEmpty()
        && m_scrollText.size() >= 4 && durs.size() == 1) {
        QVector<double> peakD;
        peakD.reserve(m_recentLetterPeaks.size());
        for (float p : m_recentLetterPeaks) {
            peakD.append(static_cast<double>(p));
        }
        const float ref = static_cast<float>(medianOf(peakD));
        const QChar u = emitChars[0].toUpper();
        const bool noiseLike = (u == QChar('I') || u == QChar('E') || u == QChar('T')
                                || u == QChar('?'));
        const double d = durs[0];
        /* Keep short E/T if duration is a plausible dit/dah for current unit */
        const bool plausibleE = (u == QChar('E') && d >= m_unitSeconds * 0.28);
        const bool plausibleT = (u == QChar('T') && d >= m_unitSeconds * 1.0);
        if (noiseLike && !plausibleE && !plausibleT
            && letterPeak < ref - 8.0f
            && letterPeak < m_thresholdDb + 2.5f) {
            return;
        }
    }

    for (int ci = 0; ci < emitChars.size(); ++ci) {
        appendChar(emitChars[ci]);
    }

    if (letterPeak > -180.0f) {
        noteMarkPeak(letterPeak);
    }
}

QString ThresholdMorseWindow::pendingMorsePreview() const
{
    QString pattern;
    const double dahMin = m_unitSeconds * 1.90;
    for (double d : m_pendingMarkDurs) {
        pattern.append(d >= dahMin ? QChar('-') : QChar('.'));
    }
    if (m_havePendingMark && m_pendingMarkSec >= 0.03) {
        pattern.append(m_pendingMarkSec >= dahMin ? QChar('-') : QChar('.'));
    }
    return pattern;
}

void ThresholdMorseWindow::noteMarkDuration(double markSeconds)
{
    /*
     * 25 WPM dit ≈ 48 ms; 30 WPM ≈ 40 ms. Keep a low floor so mid-speed
     * Auto WPM still sees dits (was 40 ms + 80–100 ms cluster cut → stuck).
     * Upper ~3 s still admits ~1–2 WPM dahs.
     */
    if (markSeconds < 0.028 || markSeconds > 3.0) {
        return;
    }
    m_allMarkSecs.append(markSeconds);
    while (m_allMarkSecs.size() > kMaxMarkHistory) {
        m_allMarkSecs.removeFirst();
    }
}

void ThresholdMorseWindow::noteLetterGap(double gapSeconds)
{
    if (gapSeconds < 0.15 || gapSeconds > 4.0) {
        return;
    }
    m_letterGaps.append(gapSeconds);
    while (m_letterGaps.size() > kMaxLetterGaps) {
        m_letterGaps.removeFirst();
    }
}

void ThresholdMorseWindow::noteElementGap(double gapSeconds)
{
    /* Element spacing ≈ 1 dit: 30 WPM ≈ 40 ms … 2 WPM ≈ 0.6 s (cap 1.5 s) */
    if (gapSeconds < 0.028 || gapSeconds > 1.50) {
        return;
    }
    m_elementGaps.append(gapSeconds);
    while (m_elementGaps.size() > kMaxElementGaps) {
        m_elementGaps.removeFirst();
    }
}

void ThresholdMorseWindow::noteMarkPeak(float peakDb)
{
    if (peakDb < -180.0f) {
        return;
    }
    m_recentLetterPeaks.append(peakDb);
    while (m_recentLetterPeaks.size() > kMaxLetterPeaks) {
        m_recentLetterPeaks.removeFirst();
    }
}

double ThresholdMorseWindow::wordMinSeconds() const
{
    const double unit = std::max(0.050, m_unitSeconds);
    if (!m_unitCalibrated) {
        return 1.20;
    }
    /*
     * ITU ideal word gap is ~7 dits, but many fists (and this capture's
     * buried "QM TU GA JE") use ~3.5–4.5× dit for words while letters are
     * ~2.5–3×. A 5.5–7× floor glued TU|GA into TUGA.
     *
     * Prefer observed letter gaps: sit modestly above the median letter gap.
     * Floor 3.5× unit; never below letter timeout (~2.6×).
     */
    double wordMin = unit * 4.0;
    if (m_letterGaps.size() >= 2) {
        const double medGap = medianOf(m_letterGaps);
        wordMin = std::max(unit * 3.5, medGap * 1.20);
        wordMin = std::min(wordMin, unit * 10.0);
    }
    wordMin = std::max(wordMin, unit * 2.80); /* above letter commit */
    return wordMin;
}

void ThresholdMorseWindow::maybeInsertProtocolSpace(QChar nextChar)
{
    /*
     * Very slow CQ fists often use the same ~3× unit gap between every
     * character — including between words — so timing alone cannot place
     * spaces (W5MHG capture: "QDEW5MHG" with uniform ~1.2 s gaps).
     * Insert conventional CQ / DE word breaks as characters arrive.
     *   QDEW5…  →  Q DE W5…
     *   CQDEW5… →  CQ DE W5…
     */
    if (nextChar == QChar(' ') || m_scrollText.isEmpty()
        || m_scrollText.endsWith(QChar(' '))) {
        return;
    }

    if (nextChar.toUpper() == QChar('D')) {
        if (m_scrollText.endsWith(QLatin1String("CQ"))
            || m_scrollText == QLatin1String("Q")
            || m_scrollText.endsWith(QLatin1String(" Q"))) {
            m_scrollText.append(QChar(' '));
        }
    }

    if (m_scrollText.endsWith(QLatin1String("DE"))
        || m_scrollText.endsWith(QLatin1String(" DE"))) {
        m_scrollText.append(QChar(' '));
    }
}

void ThresholdMorseWindow::recomputeUnitFromMarks()
{
    if (!m_autoWpmCheck || !m_autoWpmCheck->isChecked()) {
        m_unitSeconds = m_sliderUnitSeconds;
        m_unitCalibrated = true;
        return;
    }

    QVector<double> raw;
    raw.reserve(m_allMarkSecs.size() + m_pendingMarkDurs.size() + 1);
    for (double d : m_allMarkSecs) {
        raw.append(d);
    }
    for (double d : m_pendingMarkDurs) {
        raw.append(d);
    }
    if (m_havePendingMark && m_pendingMarkSec >= 0.05) {
        raw.append(m_pendingMarkSec);
    }

    /*
     * Drop noise blips AND mega-marks (threshold glitches / key-down).
     * Quality marks extend to ~2.8 s so ~1–3 WPM dahs cluster correctly.
     *
     * IMPORTANT: at 15–25 WPM, dits are ~48–80 ms and dahs ~150–250 ms.
     * The old minCluster of 80–100 ms discarded almost all dits once any
     * “long” mark was seen (and hasLongMark required 350 ms = QRS only),
     * so Auto WPM stayed stuck near the 12 WPM seed (“L RUN F” path).
     */
    const double maxQualityMarkSec = 2.80;
    double rawMax = 0.0;
    for (double d : raw) {
        if (d <= maxQualityMarkSec) {
            rawMax = std::max(rawMax, d);
        }
    }
    /* Any plausible dah (≥ ~2.2× a 40 ms noise floor, or classic ≥180 ms) */
    const bool hasDahLike = (rawMax >= 0.160);
    /* Floor just below 30 WPM dit (40 ms); QRS still uses same path */
    const double minCluster = hasDahLike ? 0.038 : 0.032;

    QVector<double> marks;
    marks.reserve(raw.size());
    for (double d : raw) {
        if (d >= minCluster && d <= maxQualityMarkSec) {
            marks.append(d);
        }
    }
    if (marks.size() < 2) {
        marks.clear();
        for (double d : raw) {
            if (d >= 0.030 && d <= maxQualityMarkSec) {
                marks.append(d);
            }
        }
    }

    /* Reject ultra-short noise once we have a stable bulk of marks */
    if (marks.size() >= 6) {
        QVector<double> sortedM = marks;
        std::sort(sortedM.begin(), sortedM.end());
        const double medM = sortedM[sortedM.size() / 2];
        const double noiseCut = std::max(minCluster, 0.32 * medM);
        QVector<double> cleaned;
        for (double d : marks) {
            if (d >= noiseCut) {
                cleaned.append(d);
            }
        }
        if (cleaned.size() >= 3) {
            marks = cleaned;
        }
    }

    /*
     * Early provisional unit (before full bimodal lock):
     *  - 1+ long marks → unit ≈ longest/3 (treat as dah)
     *  - element gaps ≈ 1 dit (strong, once we have a few)
     * This avoids classifying the first letter at the seed 12 WPM unit.
     */
    double fromGaps = 0.0;
    if (m_elementGaps.size() >= 2) {
        fromGaps = medianOf(m_elementGaps);
        if (fromGaps < 0.028 || fromGaps > 1.20) {
            fromGaps = 0.0;
        }
    }

    double fromMarks = 0.0;
    double bestRatio = 0.0;
    bool strongBimodal = false;

    if (marks.size() >= 1) {
        std::sort(marks.begin(), marks.end());
        const double maxQualityMark = marks.last();

        if (marks.size() >= 3) {
            int split = -1;
            double bestScore = -1.0;
            for (int i = 1; i < marks.size(); ++i) {
                const double gap = marks[i] - marks[i - 1];
                if (gap < 0.040) {
                    continue;
                }
                QVector<double> low;
                QVector<double> high;
                for (int j = 0; j < i; ++j) {
                    low.append(marks[j]);
                }
                for (int j = i; j < marks.size(); ++j) {
                    high.append(marks[j]);
                }
                if (low.isEmpty() || high.isEmpty()) {
                    continue;
                }
                const double medL = medianOf(low);
                const double medH = medianOf(high);
                if (medL < 0.040) {
                    continue;
                }
                const double ratio = medH / medL;
                double score = gap;
                if (ratio >= 2.2 && ratio <= 5.5) {
                    score = gap * 3.0
                            + 0.15 * (1.0 - std::min(1.0, std::fabs(ratio - 3.0) / 3.0));
                    if (ratio >= 2.5 && ratio <= 4.0) {
                        score += 0.08;
                    }
                } else if (gap < 0.10) {
                    continue;
                }
                if (score > bestScore) {
                    bestScore = score;
                    split = i;
                    bestRatio = ratio;
                }
            }

            QVector<double> dits;
            QVector<double> dahs;
            if (split > 0 && split < marks.size()) {
                for (int i = 0; i < split; ++i) {
                    dits.append(marks[i]);
                }
                for (int i = split; i < marks.size(); ++i) {
                    dahs.append(marks[i]);
                }
                strongBimodal = (bestRatio >= 2.4 && bestRatio <= 4.2);
            } else {
                /* No clear split — if all marks similar, they are dits */
                const double span = marks.last() - marks.first();
                if (span < marks.first() * 0.55) {
                    dits = marks;
                } else {
                    const int n = (marks.size() + 1) / 2;
                    for (int i = 0; i < n; ++i) {
                        dits.append(marks[i]);
                    }
                }
            }

            if (!dits.isEmpty()) {
                std::sort(dits.begin(), dits.end());
                /*
                 * Dahs-only trap: if the “low” cluster median is still ≥150 ms
                 * and we never found a real dit cluster, marks are all dahs
                 * (common when thr is high and short dits were lost).
                 */
                if (dahs.isEmpty() && medianOf(dits) >= 0.150 && medianOf(dits) <= 0.90) {
                    fromMarks = medianOf(dits) / 3.0;
                } else {
                    if (dits.size() >= 5) {
                        dits = dits.mid(dits.size() / 5);
                    }
                    if (dits.size() == 1) {
                        fromMarks = dits[0];
                    } else {
                        const int idx = std::max(0, static_cast<int>(0.45 * (dits.size() - 1)));
                        fromMarks = dits[idx];
                    }
                    if (!dahs.isEmpty() && bestRatio >= 2.2 && bestRatio <= 5.0) {
                        QVector<double> goodDahs;
                        for (double d : dahs) {
                            if (d <= 2.60) {
                                goodDahs.append(d);
                            }
                        }
                        if (!goodDahs.isEmpty()) {
                            const double fromDah = medianOf(goodDahs) / 3.0;
                            /* Allow down to ~35 ms unit (~34 WPM) */
                            if (fromDah >= 0.032 && fromDah <= 1.20) {
                                fromMarks = 0.55 * fromMarks + 0.45 * fromDah;
                            }
                        }
                    }
                }
            }
        } else if (marks.size() == 2) {
            const double a = marks[0];
            const double b = marks[1];
            const double ratio = b / std::max(0.001, a);
            if (ratio >= 2.2 && ratio <= 5.0) {
                fromMarks = a; /* short is dit */
                strongBimodal = true;
            } else {
                /* Two similar marks: dits, or two dahs → /3 */
                const double mid = 0.5 * (a + b);
                fromMarks = (mid >= 0.45) ? (mid / 3.0) : mid;
            }
        } else {
            /* Single mark: if long, treat as dah */
            fromMarks = (marks[0] >= 0.30) ? (marks[0] / 3.0) : marks[0];
        }

        if (maxQualityMark >= 0.70 && fromMarks > 0.0) {
            fromMarks = std::max(fromMarks, maxQualityMark / 3.5);
        } else if (maxQualityMark >= 0.50 && fromMarks > 0.0) {
            fromMarks = std::max(fromMarks, maxQualityMark / 4.0);
        }
    }

    /* Blend mark-based and element-gap-based estimates */
    double med = 0.0;
    if (fromMarks > 0.0 && fromGaps > 0.0) {
        /* Gaps are often slightly longer than pure dits — light blend */
        med = 0.62 * fromMarks + 0.38 * fromGaps;
        /* If they disagree a lot, prefer marks (gaps can include fist stretch) */
        if (fromGaps > fromMarks * 1.8 || fromMarks > fromGaps * 1.8) {
            med = 0.80 * fromMarks + 0.20 * fromGaps;
        }
    } else if (fromMarks > 0.0) {
        med = fromMarks;
    } else if (fromGaps > 0.0) {
        med = fromGaps;
    } else {
        return;
    }

    /* ~34 WPM … ~1 WPM */
    if (med < 0.032 || med > 1.25) {
        return;
    }

    if (!m_unitCalibrated) {
        m_unitSeconds = med;
        m_unitCalibrated = true;
    } else {
        /*
         * Fast track when a clear dit/dah split agrees with gaps; otherwise
         * slower so one noisy letter does not yank WPM.
         * Speeding up (12→18 WPM) must be quick — seed 12 WPM otherwise
         * freezes and mid-speed fists decode as garbage.
         */
        double alpha = 0.30;
        if (strongBimodal) {
            alpha = 0.50;
        }
        if (fromGaps > 0.0 && fromMarks > 0.0
            && std::fabs(fromGaps - fromMarks) < 0.40 * med) {
            alpha = std::max(alpha, 0.45);
        }
        if (m_unitSeconds > 0.001) {
            const double ratio = med / m_unitSeconds;
            /* Slowing down a lot (QRS) or speeding up a lot (12→20 WPM) */
            if (ratio > 1.55 || ratio < 0.70) {
                alpha = std::max(alpha, 0.62);
            }
            /* Prefer following a clearly shorter unit (faster code) */
            if (ratio < 0.85) {
                alpha = std::max(alpha, 0.48);
            }
        }
        m_unitSeconds = (1.0 - alpha) * m_unitSeconds + alpha * med;
    }
    m_unitSeconds = std::max(0.032, std::min(1.20, m_unitSeconds));

    /* Reflect Auto estimate on the slider so the dark WPM box stays truthful */
    if (m_wpmSlider) {
        const int w = std::max(1, std::min(40, static_cast<int>(std::lround(1.2 / m_unitSeconds))));
        if (m_wpmSlider->value() != w) {
            m_wpmSlider->blockSignals(true);
            m_wpmSlider->setValue(w);
            m_wpmSlider->blockSignals(false);
            m_sliderUnitSeconds = ditSecondsForWpm(w);
        }
    }
}

void ThresholdMorseWindow::appendChar(QChar ch)
{
    if (ch == QChar(' ') && (m_scrollText.isEmpty() || m_scrollText.endsWith(QChar(' ')))) {
        return;
    }

    if (ch != QChar(' ')) {
        maybeInsertProtocolSpace(ch);

        int te = 0;
        for (const QChar &c : m_recentChars) {
            if (c == QChar('T') || c == QChar('E')) {
                te++;
            }
        }
        const int n = m_recentChars.size();
        if (n >= 10 && te * 100 / n >= 80 && (ch == QChar('T') || ch == QChar('E'))) {
            m_spamHintCount = std::min(10, m_spamHintCount + 1);
            return;
        }

        m_recentChars.append(ch.toUpper());
        while (m_recentChars.size() > kMaxRecentChars) {
            m_recentChars.remove(0, 1);
        }
        te = 0;
        for (const QChar &c : m_recentChars) {
            if (c == QChar('T') || c == QChar('E')) {
                te++;
            }
        }
        if (m_recentChars.size() >= 8 && te * 100 / m_recentChars.size() >= 75) {
            m_spamHintCount = std::min(10, m_spamHintCount + 1);
        } else {
            m_spamHintCount = std::max(0, m_spamHintCount - 1);
        }
    }

    m_scrollText.append(ch);
    trimScrollBuffer();
    refreshTextView();
    emit textChanged(m_scrollText);
}

void ThresholdMorseWindow::trimScrollBuffer()
{
    if (m_scrollText.size() <= kMaxScrollChars) {
        return;
    }
    const int drop = m_scrollText.size() - kMaxScrollChars + 200;
    int cut = drop;
    const int sp = m_scrollText.indexOf(QChar(' '), std::max(0, drop - 40));
    if (sp >= 0 && sp < drop + 80) {
        cut = sp + 1;
    }
    m_scrollText.remove(0, cut);
}

void ThresholdMorseWindow::refreshTextView()
{
    if (!m_textView) {
        return;
    }
    if (m_textView->toPlainText() != m_scrollText) {
        m_textView->setPlainText(m_scrollText);
    }
    QTextCursor c = m_textView->textCursor();
    c.movePosition(QTextCursor::End);
    m_textView->setTextCursor(c);
    m_textView->ensureCursorVisible();
}

char ThresholdMorseWindow::morseToChar(const QString &pattern)
{
    const auto &table = morseTable();
    const auto it = table.find(pattern);
    return (it != table.end()) ? it->second : 0;
}

void ThresholdMorseWindow::clearDecode()
{
    m_scrollText.clear();
    emit textChanged(m_scrollText);
    m_pendingMarkDurs.clear();
    m_pendingMarkPeaks.clear();
    m_recentChars.clear();
    m_spamHintCount = 0;
    m_havePendingMark = false;
    m_pendingMarkSec = 0.0;
    m_pendingMarkPeakDb = -200.0f;
    m_letterEmittedForSpace = false;
    if (m_textView) {
        m_textView->clear();
    }
    updateUi();
}

void ThresholdMorseWindow::resetTiming()
{
    applyWpmFromSlider();
    m_unitSeconds = m_sliderUnitSeconds;
    /* Manual WPM: stay calibrated so letter gaps use unit×2.1 immediately.
     * Auto WPM / headless multi-channel: recalibrate from the next marks. */
    if (m_headless) {
        m_unitCalibrated = false;
    } else {
        m_unitCalibrated = !(m_autoWpmCheck && m_autoWpmCheck->isChecked());
    }
    m_allMarkSecs.clear();
    m_letterGaps.clear();
    m_elementGaps.clear();
    m_recentLetterPeaks.clear();
    m_hasState = false;
    m_pendingMarkDurs.clear();
    m_pendingMarkPeaks.clear();
    m_havePendingMark = false;
    m_pendingMarkSec = 0.0;
    m_pendingMarkPeakDb = -200.0f;
    m_letterEmittedForSpace = false;
    m_inValley = false;
    m_valleyStartMs = -1;
    m_segPeakDb = -200.0f;
    m_markCount = 0;
    m_spaceCount = 0;
    m_squelchOpen = m_playbackMode;  /* stay open across reset during playback */
    m_markEnergySec = 0.0;
    m_peakAboveThreshDb = -100.0f;
    m_spamHintCount = 0;
    updateUi();
}

void ThresholdMorseWindow::setPlaybackMode(bool on)
{
    m_playbackMode = on;
    if (on) {
        m_squelchOpen = true;
    }
}

void ThresholdMorseWindow::prepareForPlayback()
{
    m_playbackMode = true;
    clearDecode();
    resetTiming();
    m_squelchOpen = true;
    m_sampleCount = 0;
    if (m_textView) {
        m_textView->setPlaceholderText(
            QString("Playback — decoding… (code speed %1 WPM or Auto)")
                .arg(wpm()));
    }
    updateUi();
}

void ThresholdMorseWindow::onClearClicked()
{
    clearDecode();
}

void ThresholdMorseWindow::onResetTimingClicked()
{
    resetTiming();
}

void ThresholdMorseWindow::onWpmSliderChanged(int /*wpm*/)
{
    applyWpmFromSlider();
    updateUi();
}

void ThresholdMorseWindow::onAutoToggled(bool /*checked*/)
{
    applyWpmFromSlider();
    updateUi();
}

void ThresholdMorseWindow::updateUi()
{
    if (m_headless) {
        return;
    }
    const double wpmEff = (m_unitSeconds > 0.001) ? (1.2 / m_unitSeconds) : 0.0;
    const qint64 holdMs = m_hasState ? (m_lastSampleMs - m_stateStartMs) : 0;
    const bool autoOn = m_autoWpmCheck && m_autoWpmCheck->isChecked();

    if (m_statusLabel) {
        const QString state = !m_hasState ? "IDLE" : (m_isHigh ? "HIGH" : "LOW");
        const QString sq = m_squelchOpen ? "OPEN" : "SQUELCH";
        m_statusLabel->setText(
            QString("%1   |   thresh %2 dB   |   now %3 dB (%4)   |   %5   |   CW %6")
                .arg(m_targetLabel.isEmpty() ? QString("Trace") : m_targetLabel)
                .arg(m_thresholdDb, 0, 'f', 1)
                .arg(m_lastPowerDb, 0, 'f', 1)
                .arg(m_lastPowerDb - m_thresholdDb, 0, 'f', 1)
                .arg(state)
                .arg(sq));
    }

    if (m_elementLabel) {
        if (!m_squelchOpen) {
            m_elementLabel->setText("SQUELCH — waiting for strong CW keying…");
        } else {
            const QString prev = pendingMorsePreview();
            m_elementLabel->setText(
                QString("Building: [%1]%2")
                    .arg(prev.isEmpty() ? QString("—") : prev)
                    .arg(m_unitCalibrated ? QString() : QString("  (calibrating WPM…)")));
        }
    }

    QString hint;
    if (m_spamHintCount >= 1) {
        hint = "  |  HINT: many T/E → WPM wrong or noise";
    }

    if (m_timingLabel) {
        m_timingLabel->setText(
            QString("%1  dit=%2 ms (~%3 WPM)%4  hold=%5 ms  marks=%6 spaces=%7  "
                    "energy=%8 s%9")
                .arg(autoOn ? "AUTO" : "MANUAL")
                .arg(m_unitSeconds * 1000.0, 0, 'f', 0)
                .arg(wpmEff, 0, 'f', 1)
                .arg(m_unitCalibrated ? "" : " ~est")
                .arg(holdMs)
                .arg(m_markCount)
                .arg(m_spaceCount)
                .arg(m_markEnergySec, 0, 'f', 2)
                .arg(hint));
    }

    if (m_wpmLabel) {
        /* Dark-lettered primary WPM; dit length as secondary detail */
        if (autoOn) {
            if (m_unitCalibrated) {
                m_wpmLabel->setText(
                    QString("%1 WPM\ndit %2 ms")
                        .arg(wpmEff, 0, 'f', 0)
                        .arg(m_unitSeconds * 1000.0, 0, 'f', 0));
            } else {
                m_wpmLabel->setText(
                    QString("%1 WPM\nauto…")
                        .arg(wpmEff, 0, 'f', 0));
            }
        } else {
            m_wpmLabel->setText(
                QString("%1 WPM\nslider")
                    .arg(static_cast<double>(wpm()), 0, 'f', 0));
        }
    }
}
