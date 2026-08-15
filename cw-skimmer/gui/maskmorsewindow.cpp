/**
 * @file maskmorsewindow.cpp
 * @brief Trapezoidal dit/dah geometric mask Morse decoder
 */

#include "maskmorsewindow.h"

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
        {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-...-", '='},
        {"-....-", '-'}, {"-..-.", '/'},  {".-.-.", '+'},
    };
    return table;
}

double ditSecondsForWpm(double wpm)
{
    const double w = std::max(1.0, std::min(80.0, wpm));
    return 1.2 / w;
}

}  // namespace

/* ---------- MaskMorseWindow ---------- */

MaskMorseWindow::MaskMorseWindow(QWidget *parent, bool headless)
    : QWidget(parent)
    , m_headless(headless)
    , m_statusLabel(nullptr)
    , m_maskLabel(nullptr)
    , m_wpmValueLabel(nullptr)
    , m_ditWidthLabel(nullptr)
    , m_dahWidthLabel(nullptr)
    , m_textView(nullptr)
    , m_clearButton(nullptr)
    , m_resetTimingButton(nullptr)
    , m_autoWpmCheck(nullptr)
    , m_autoWidthCheck(nullptr)
    , m_wpmSlider(nullptr)
    , m_ditWidthSlider(nullptr)
    , m_dahWidthSlider(nullptr)
    , m_thresholdDb(-70.0f)
    , m_noiseFloorDb(-90.0f)
    , m_peakHoldDb(-200.0f)
    , m_lastDitScore(0.0f)
    , m_lastDahScore(0.0f)
    , m_looking(true)
    , m_ditHit(false)
    , m_dahHit(false)
    , m_hitHoldUntilMs(0)
    , m_unitSeconds(0.08)
    , m_sliderUnitSeconds(0.08)
    , m_ditWidthSec(0.08)
    , m_dahWidthSec(0.24)
    , m_measDitSec(0.08)
    , m_measDahSec(0.24)
    , m_localDitSec(0.08)
    , m_localDahSec(0.24)
    , m_localPeakDb(-200.0f)
    , m_localFloorDb(-90.0f)
    , m_localReady(false)
    , m_unitCalibrated(false)
    , m_autoWpm(true)
    , m_autoWidth(true)
    , m_keyHigh(false)
    , m_hasState(false)
    , m_markStartMs(0)
    , m_lastElementEndMs(-1)
    , m_lastSampleMs(-1)
    , m_onsetHistIdx(-1)
    , m_matchedThisMark(false)
    , m_markPeakDb(-200.0f)
    , m_lastPowerDb(-200.0f)
    , m_segPeakDb(-200.0f)
    , m_inValley(false)
    , m_valleyStartMs(-1)
    , m_valleyNeedsMarkPeak(false)
    , m_digitGroupRemain(0)
{
    m_clock.start();
    m_sliderUnitSeconds = ditSecondsForWpm(15.0);
    m_unitSeconds = m_sliderUnitSeconds;
    m_ditWidthSec = m_unitSeconds;
    m_dahWidthSec = 3.0 * m_unitSeconds;
    m_measDitSec = m_ditWidthSec;
    m_measDahSec = m_dahWidthSec;
    m_localDitSec = m_ditWidthSec;
    m_localDahSec = m_dahWidthSec;

    if (m_headless) {
        return;
    }

    /* No live status / "Pending:" banner — it resized every sample and jumped the UI. */
    m_statusLabel = nullptr;
    m_maskLabel = nullptr;

    m_textView = new QPlainTextEdit(this);
    m_textView->setReadOnly(true);
    m_textView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_textView->setPlaceholderText(
        "Mask Morse: pulse fill (rise→fall). Look-ahead ~4 dahs pre-sets\n"
        "mask height from upcoming dah tops; widths local. RED=looking GREEN=hit.");
    m_textView->setStyleSheet(
        "QPlainTextEdit { background: #0a0a0c; color: #88ffaa; font-family: Courier; "
        "font-size: 18px; }");

    m_clearButton = new QPushButton("Clear Text", this);
    m_resetTimingButton = new QPushButton("Reset Timing", this);
    m_autoWpmCheck = new QCheckBox("Auto WPM", this);
    m_autoWpmCheck->setChecked(true);
    /* 0.1 WPM steps: slider value = WPM × 10  (5.0 … 40.0) */
    m_wpmSlider = new QSlider(Qt::Horizontal, this);
    m_wpmSlider->setRange(50, 400);
    m_wpmSlider->setValue(150);
    m_wpmSlider->setSingleStep(1);
    m_wpmSlider->setPageStep(5);
    m_wpmSlider->setTickInterval(50);
    m_wpmSlider->setTickPosition(QSlider::TicksBelow);
    m_wpmSlider->setToolTip(
        QStringLiteral("Code speed seed (0.1 WPM). Used for letter gaps and as "
                       "fallback before mark lengths are measured."));
    m_wpmValueLabel = new QLabel(QStringLiteral("15.0 WPM"), this);
    m_wpmValueLabel->setMinimumWidth(72);
    m_wpmValueLabel->setAlignment(Qt::AlignCenter);
    m_wpmValueLabel->setStyleSheet(
        "QLabel { font-weight: bold; font-family: Courier; font-size: 13px; }");

    m_autoWidthCheck = new QCheckBox("Auto widths", this);
    m_autoWidthCheck->setChecked(true);
    m_autoWidthCheck->setToolTip(
        QStringLiteral("When checked, dit/dah box widths track measured mark lengths "
                       "from the threshold keying stream (short cluster = dit, long = dah). "
                       "Uncheck to set Dit/Dah ms sliders independently."));

    m_ditWidthSlider = new QSlider(Qt::Horizontal, this);
    m_ditWidthSlider->setRange(20, 600);   /* ms */
    m_ditWidthSlider->setValue(80);
    m_ditWidthSlider->setSingleStep(1);
    m_ditWidthSlider->setPageStep(5);
    m_ditWidthSlider->setToolTip(QStringLiteral("Dit mask width in milliseconds (independent of dah)."));
    m_ditWidthLabel = new QLabel(QStringLiteral("Dit 80 ms"), this);
    m_ditWidthLabel->setMinimumWidth(88);
    m_ditWidthLabel->setAlignment(Qt::AlignCenter);
    m_ditWidthLabel->setStyleSheet(
        "QLabel { font-weight: bold; font-family: Courier; font-size: 12px; color: #a20; }");

    m_dahWidthSlider = new QSlider(Qt::Horizontal, this);
    m_dahWidthSlider->setRange(40, 1800);  /* ms */
    m_dahWidthSlider->setValue(240);
    m_dahWidthSlider->setSingleStep(2);
    m_dahWidthSlider->setPageStep(10);
    m_dahWidthSlider->setToolTip(QStringLiteral("Dah mask width in milliseconds (independent of dit)."));
    m_dahWidthLabel = new QLabel(QStringLiteral("Dah 240 ms"), this);
    m_dahWidthLabel->setMinimumWidth(96);
    m_dahWidthLabel->setAlignment(Qt::AlignCenter);
    m_dahWidthLabel->setStyleSheet(
        "QLabel { font-weight: bold; font-family: Courier; font-size: 12px; color: #a20; }");

    auto *row = new QHBoxLayout();
    row->addWidget(m_autoWpmCheck);
    row->addWidget(new QLabel("WPM", this));
    row->addWidget(m_wpmSlider, 1);
    row->addWidget(m_wpmValueLabel);
    row->addWidget(m_resetTimingButton);
    row->addWidget(m_clearButton);

    auto *widthRow = new QHBoxLayout();
    widthRow->addWidget(m_autoWidthCheck);
    widthRow->addWidget(new QLabel("Dit", this));
    widthRow->addWidget(m_ditWidthSlider, 1);
    widthRow->addWidget(m_ditWidthLabel);
    widthRow->addWidget(new QLabel("Dah", this));
    widthRow->addWidget(m_dahWidthSlider, 1);
    widthRow->addWidget(m_dahWidthLabel);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_textView, 1);
    layout->addLayout(row);
    layout->addLayout(widthRow);
    setLayout(layout);

    connect(m_clearButton, &QPushButton::clicked, this, &MaskMorseWindow::onClearClicked);
    connect(m_resetTimingButton, &QPushButton::clicked, this,
            &MaskMorseWindow::onResetTimingClicked);
    connect(m_wpmSlider, &QSlider::valueChanged, this, &MaskMorseWindow::onWpmSliderChanged);
    connect(m_autoWpmCheck, &QCheckBox::toggled, this, &MaskMorseWindow::onAutoToggled);
    connect(m_autoWidthCheck, &QCheckBox::toggled, this, &MaskMorseWindow::onAutoWidthToggled);
    connect(m_ditWidthSlider, &QSlider::valueChanged, this,
            &MaskMorseWindow::onDitWidthSliderChanged);
    connect(m_dahWidthSlider, &QSlider::valueChanged, this,
            &MaskMorseWindow::onDahWidthSliderChanged);

    updateUi();
}

double MaskMorseWindow::medianOf(QVector<double> v)
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

char MaskMorseWindow::morseToChar(const QString &pattern)
{
    const auto &table = morseTable();
    const auto it = table.find(pattern);
    return (it != table.end()) ? it->second : 0;
}

int MaskMorseWindow::wpm() const
{
    return static_cast<int>(std::lround(wpmF()));
}

double MaskMorseWindow::wpmF() const
{
    if (m_unitSeconds > 1e-6) {
        return 1.2 / m_unitSeconds;
    }
    if (m_wpmSlider) {
        return m_wpmSlider->value() / 10.0;
    }
    return 15.0;
}

QString MaskMorseWindow::trailingText(int maxChars) const
{
    if (maxChars <= 0 || m_scrollText.isEmpty()) {
        return QString();
    }
    if (m_scrollText.size() <= maxChars) {
        return m_scrollText;
    }
    return m_scrollText.right(maxChars);
}

void MaskMorseWindow::applyWpmFromSlider()
{
    if (!m_wpmSlider) {
        return;
    }
    /* Slider is WPM × 10 (0.1 WPM steps). */
    const double wpmVal = m_wpmSlider->value() / 10.0;
    m_sliderUnitSeconds = ditSecondsForWpm(wpmVal);
    if (!m_autoWpm) {
        m_unitSeconds = m_sliderUnitSeconds;
        m_unitCalibrated = true;
        /* WPM seed only fills widths when Auto widths is also off and unset */
        if (!m_autoWidth) {
            /* leave dit/dah sliders as the authority */
        } else {
            m_ditWidthSec = m_unitSeconds;
            m_dahWidthSec = 3.0 * m_unitSeconds;
            m_measDitSec = m_ditWidthSec;
            m_measDahSec = m_dahWidthSec;
            syncWidthSlidersFromEffective();
        }
    }
    if (m_wpmValueLabel) {
        m_wpmValueLabel->setText(QStringLiteral("%1 WPM").arg(wpmVal, 0, 'f', 1));
    }
}

void MaskMorseWindow::applyWidthSliders()
{
    if (!m_ditWidthSlider || !m_dahWidthSlider) {
        return;
    }
    m_ditWidthSec = std::max(0.015, m_ditWidthSlider->value() / 1000.0);
    m_dahWidthSec = std::max(m_ditWidthSec + 0.010, m_dahWidthSlider->value() / 1000.0);
    /* Grouping unit tracks dit width */
    m_unitSeconds = m_ditWidthSec;
    m_unitCalibrated = true;
    if (m_ditWidthLabel) {
        m_ditWidthLabel->setText(
            QStringLiteral("Dit %1 ms").arg(m_ditWidthSec * 1000.0, 0, 'f', 0));
    }
    if (m_dahWidthLabel) {
        m_dahWidthLabel->setText(
            QStringLiteral("Dah %1 ms").arg(m_dahWidthSec * 1000.0, 0, 'f', 0));
    }
}

void MaskMorseWindow::syncWidthSlidersFromEffective()
{
    if (!m_ditWidthSlider || !m_dahWidthSlider) {
        return;
    }
    const int ditMs = static_cast<int>(std::lround(m_ditWidthSec * 1000.0));
    const int dahMs = static_cast<int>(std::lround(m_dahWidthSec * 1000.0));
    const int ditClamped = std::max(m_ditWidthSlider->minimum(),
                                    std::min(m_ditWidthSlider->maximum(), ditMs));
    const int dahClamped = std::max(m_dahWidthSlider->minimum(),
                                    std::min(m_dahWidthSlider->maximum(), dahMs));
    m_ditWidthSlider->blockSignals(true);
    m_dahWidthSlider->blockSignals(true);
    m_ditWidthSlider->setValue(ditClamped);
    m_dahWidthSlider->setValue(dahClamped);
    m_ditWidthSlider->blockSignals(false);
    m_dahWidthSlider->blockSignals(false);
    if (m_ditWidthLabel) {
        m_ditWidthLabel->setText(
            QStringLiteral("Dit %1 ms").arg(m_ditWidthSec * 1000.0, 0, 'f', 0));
    }
    if (m_dahWidthLabel) {
        m_dahWidthLabel->setText(
            QStringLiteral("Dah %1 ms").arg(m_dahWidthSec * 1000.0, 0, 'f', 0));
    }
}

double MaskMorseWindow::estimateDtSec() const
{
    double dtSec = 0.02;
    if (m_timeHist.size() >= 4) {
        const int n = m_timeHist.size();
        const qint64 span = m_timeHist[n - 1] - m_timeHist[std::max(0, n - 8)];
        const int steps = std::min(7, n - 1);
        if (span > 0 && steps > 0) {
            dtSec = (span / 1000.0) / static_cast<double>(steps);
            dtSec = std::max(0.008, std::min(0.08, dtSec));
        }
    }
    return dtSec;
}

double MaskMorseWindow::effectiveDitSec() const
{
    if (m_autoWidth && m_localReady) {
        return std::max(0.025, m_localDitSec);
    }
    return std::max(0.025, m_ditWidthSec);
}

double MaskMorseWindow::effectiveDahSec() const
{
    const double dit = effectiveDitSec();
    if (m_autoWidth && m_localReady) {
        return std::max(dit + 0.015, m_localDahSec);
    }
    return std::max(dit + 0.015, m_dahWidthSec);
}

float MaskMorseWindow::effectivePeakDb() const
{
    if (m_localReady && m_localPeakDb > -180.0f) {
        return m_localPeakDb;
    }
    if (m_peakHoldDb > -180.0f) {
        return m_peakHoldDb;
    }
    return m_thresholdDb + 12.0f;
}

void MaskMorseWindow::updateLocalMaskFromLookahead()
{
    /*
     * Look-ahead = 4 × dah ahead of DETECT (newest samples toward "now").
     * Height is pre-set from dah-class mark tops in that window so the mask
     * works at the level it will encounter — not overwhelmed by a single
     * global max or by short dit peaks.
     *
     * ~10 WPM: dah≈0.38 s → 4 dahs ≈ 1.5 s look-ahead.
     */
    const int n = m_powerHist.size();
    if (n < 12) {
        return;
    }

    const double seedDah = std::max(0.12, m_localReady ? m_localDahSec : m_dahWidthSec);
    const double seedDit = std::max(0.040, m_localReady ? m_localDitSec : m_ditWidthSec);
    const double lookSec = kLookaheadDahs * seedDah;
    const double dt = estimateDtSec();
    const int lookN = std::max(30, static_cast<int>(std::lround(lookSec / dt)));
    const int start = std::max(0, n - lookN);

    /* Reconstruct key-high pulses in the look-ahead window using thr */
    struct LookMark {
        double dur;
        float peak;
    };
    QVector<LookMark> lookMarks;
    bool high = false;
    int markStart = start;
    float markPeak = -200.0f;

    for (int i = start; i < n; ++i) {
        const bool ah = m_powerHist[i] >= m_thresholdDb;
        if (ah && !high) {
            high = true;
            markStart = i;
            markPeak = m_powerHist[i];
        } else if (ah && high) {
            markPeak = std::max(markPeak, m_powerHist[i]);
        } else if (!ah && high) {
            high = false;
            const double dur = (i - markStart) * dt;
            if (dur >= 0.022 && dur <= 0.90) {
                lookMarks.append({dur, markPeak});
            }
        }
    }
    /* Open mark: height only if already dah-length */
    float openDahPeak = -200.0f;
    if (high) {
        const double dur = (n - markStart) * dt;
        if (dur >= seedDit * 1.8) {
            openDahPeak = markPeak;
        }
        if (dur >= 0.022 && dur <= 0.90) {
            lookMarks.append({dur, markPeak});
        }
    }

    QVector<double> marks;
    marks.reserve(lookMarks.size());
    for (const LookMark &lm : lookMarks) {
        marks.append(lm.dur);
    }

    /* Local floor from key-up in look-ahead */
    QVector<float> lows;
    for (int i = start; i < n; ++i) {
        if (m_powerHist[i] < m_thresholdDb) {
            lows.append(m_powerHist[i]);
        }
    }
    if (lows.size() >= 4) {
        std::sort(lows.begin(), lows.end());
        m_localFloorDb = lows[lows.size() / 4];
    } else if (m_noiseFloorDb > -180.0f) {
        m_localFloorDb = m_noiseFloorDb;
    }

    if (marks.isEmpty()) {
        if (openDahPeak > -180.0f) {
            m_localPeakDb = (m_localPeakDb < -180.0f)
                                ? openDahPeak
                                : (0.40f * m_localPeakDb + 0.60f * openDahPeak);
            m_localReady = true;
        }
        return;
    }

    std::sort(marks.begin(), marks.end());
    double ditEst = 0.0;
    double dahEst = 0.0;
    QVector<double> shorts;
    QVector<double> longs;

    if (marks.size() == 1) {
        if (marks[0] >= 0.14) {
            dahEst = marks[0];
            ditEst = marks[0] / 3.0;
            longs.append(marks[0]);
        } else {
            ditEst = marks[0];
            dahEst = 3.0 * marks[0];
            shorts.append(marks[0]);
        }
    } else {
        const int pIdx = std::max(0, static_cast<int>(0.30 * (marks.size() - 1)));
        const double p30 = marks[pIdx];
        double cut = p30 * 1.70;
        /* Prefer gap split when ratio is classic 1.8–4.5× */
        int bestSplit = -1;
        double bestScore = -1.0;
        for (int i = 1; i < marks.size(); ++i) {
            const double gap = marks[i] - marks[i - 1];
            const double ratio = marks[i] / std::max(1e-4, marks[i - 1]);
            if (ratio >= 1.75 && ratio <= 5.0 && gap * ratio > bestScore) {
                bestScore = gap * ratio;
                bestSplit = i;
            }
        }
        if (bestSplit > 0) {
            cut = 0.5 * (marks[bestSplit - 1] + marks[bestSplit]);
        }
        for (double d : marks) {
            if (d <= cut) {
                shorts.append(d);
            } else {
                longs.append(d);
            }
        }
        if (!shorts.isEmpty() && !longs.isEmpty()) {
            ditEst = medianOf(shorts);
            dahEst = medianOf(longs);
            if (dahEst / std::max(1e-4, ditEst) < 2.0 || ditEst >= 0.14) {
                const double body = medianOf(marks);
                if (body >= 0.14) {
                    dahEst = body;
                    ditEst = body / 3.0;
                    longs = marks; /* treat body as dah for height */
                    shorts.clear();
                }
            }
        } else if (!longs.isEmpty()) {
            dahEst = medianOf(longs);
            ditEst = dahEst / 3.0;
        } else {
            const double body = medianOf(shorts.isEmpty() ? marks : shorts);
            if (body >= 0.14) {
                dahEst = body;
                ditEst = body / 3.0;
                longs = shorts;
                shorts.clear();
            } else {
                ditEst = body;
                dahEst = 3.0 * body;
            }
        }
    }

    ditEst = std::max(0.030, std::min(0.22, ditEst));
    /*
     * Cap dah width so one stuck/long mark (0.7 s+) does not overwhelm
     * the local dah template — use median of longs only (already) and clamp.
     */
    dahEst = std::max(ditEst * 2.2, std::min(0.65, dahEst));

    /* Width blend — moderate so dah isn't yanked by one outlier */
    const double aW = marks.size() >= 3 ? 0.35 : 0.22;
    m_localDitSec = (1.0 - aW) * m_localDitSec + aW * ditEst;
    m_localDahSec = (1.0 - aW) * m_localDahSec + aW * dahEst;
    m_localDitSec = std::max(0.030, std::min(0.22, m_localDitSec));
    m_localDahSec = std::max(m_localDitSec * 2.2, std::min(0.65, m_localDahSec));

    /*
     * Height from dah-class mark tops only (duration ≥ ~1.8× local dit).
     * Use upper-mid of those peaks — not window max — so a single loud spike
     * or short dit cannot overwhelm the height DETECT will meet.
     */
    const double dahGate = std::max(ditEst * 1.8, m_localDitSec * 1.8);
    const double midGate = 0.5 * (ditEst + dahEst);
    QVector<float> dahPeaks;
    QVector<float> allPeaks;
    for (const LookMark &lm : lookMarks) {
        allPeaks.append(lm.peak);
        if (lm.dur >= dahGate) {
            dahPeaks.append(lm.peak);
        }
    }
    if (dahPeaks.isEmpty()) {
        for (const LookMark &lm : lookMarks) {
            if (lm.dur >= midGate) {
                dahPeaks.append(lm.peak);
            }
        }
    }
    if (openDahPeak > -180.0f) {
        dahPeaks.append(openDahPeak);
    }

    float heightTarget = -200.0f;
    if (!dahPeaks.isEmpty()) {
        std::sort(dahPeaks.begin(), dahPeaks.end());
        const int hi = std::min(dahPeaks.size() - 1,
                                static_cast<int>(0.75 * (dahPeaks.size() - 1)));
        heightTarget = dahPeaks[hi];
    } else if (!allPeaks.isEmpty()) {
        std::sort(allPeaks.begin(), allPeaks.end());
        heightTarget = allPeaks[allPeaks.size() / 2];
    }

    if (heightTarget > -180.0f) {
        if (m_localPeakDb < -180.0f) {
            m_localPeakDb = heightTarget;
        } else {
            /* Track toward upcoming dah height (slightly fast so mask is ready) */
            m_localPeakDb = 0.35f * m_localPeakDb + 0.65f * heightTarget;
        }
    }

    m_localReady = true;

    if (m_autoWidth) {
        m_ditWidthSec = 0.88 * m_ditWidthSec + 0.12 * m_localDitSec;
        m_dahWidthSec = 0.88 * m_dahWidthSec + 0.12 * m_localDahSec;
        m_measDitSec = m_localDitSec;
        m_measDahSec = m_localDahSec;
        m_unitSeconds = m_localDitSec;
        m_unitCalibrated = true;
        syncWidthSlidersFromEffective();
    }
}

void MaskMorseWindow::setTargetLabel(const QString &label)
{
    m_targetLabel = label;
    updateUi();
}

void MaskMorseWindow::setThresholdDb(float thresholdDb)
{
    m_thresholdDb = thresholdDb;
}

void MaskMorseWindow::setNoiseFloorDb(float noiseFloorDb)
{
    m_noiseFloorDb = noiseFloorDb;
}

float MaskMorseWindow::maskHeightAt(float tNorm, float hStart, float hEnd, float riseFrac,
                                    float fallFrac) const
{
    /* tNorm in [0,1] along the mask width */
    if (tNorm <= 0.0f) {
        return 0.0f;
    }
    if (tNorm >= 1.0f) {
        return 0.0f;
    }
    const float rise = std::max(0.05f, std::min(0.35f, riseFrac));
    const float fall = std::max(0.05f, std::min(0.35f, fallFrac));
    const float plateauEnd = 1.0f - fall;

    float hPlateau;
    if (tNorm < rise) {
        const float a = tNorm / rise;
        /* Ramp from 0 to interpolated height at end of rise */
        const float hAtRise = hStart + (hEnd - hStart) * rise;
        return a * hAtRise;
    }
    if (tNorm > plateauEnd) {
        const float a = (1.0f - tNorm) / fall;
        const float hAtFall = hStart + (hEnd - hStart) * plateauEnd;
        return a * hAtFall;
    }
    /* Plateau with linear slant hStart → hEnd across full width */
    hPlateau = hStart + (hEnd - hStart) * tNorm;
    return hPlateau;
}

float MaskMorseWindow::pulseBaseDb(int onsetIdx) const
{
    float baseDb = m_noiseFloorDb;
    if (baseDb < -180.0f) {
        baseDb = m_thresholdDb - 15.0f;
    }
    if (onsetIdx <= 0 || m_powerHist.isEmpty()) {
        return baseDb;
    }
    float histMin = m_powerHist[std::max(0, onsetIdx - 1)];
    const int lookBack = std::min(onsetIdx, 40);
    for (int i = onsetIdx - lookBack; i < onsetIdx; ++i) {
        if (i >= 0) {
            histMin = std::min(histMin, m_powerHist[i]);
        }
    }
    if (histMin < baseDb + 20.0f) {
        baseDb = std::min(baseDb, histMin);
    }
    return baseDb;
}

float MaskMorseWindow::pulsePeakHeight(int onsetIdx, int endIdx, float baseDb) const
{
    float hMax = 0.0f;
    const int lo = std::max(0, onsetIdx);
    const int hi = std::min(m_powerHist.size(), endIdx);
    for (int i = lo; i < hi; ++i) {
        hMax = std::max(hMax, m_powerHist[i] - baseDb);
    }
    return hMax;
}

float MaskMorseWindow::scorePulseFillMask(int onsetIdx, int endIdx, int maskWidthSamples,
                                          float baseDb, float hPeak) const
{
    /*
     * Pulse = rising→falling key-high region [onsetIdx, endIdx).
     * Mask = fixed-width template left-aligned at onset, height ≈ pulse peak.
     *
     * Hit = pulse energy mostly fills the mask AND does not leave a large
     * spill outside the mask (long pulse vs short dit box).
     */
    if (onsetIdx < 0 || endIdx <= onsetIdx || maskWidthSamples < 3) {
        return 0.0f;
    }
    if (hPeak < 1.0f) {
        return 0.0f;
    }
    if (onsetIdx >= m_powerHist.size()) {
        return 0.0f;
    }

    const int pulseW = endIdx - onsetIdx;
    const int span = std::max(pulseW, maskWidthSamples);
    const float invMask = 1.0f / static_cast<float>(maskWidthSamples - 1);

    double overlap = 0.0;
    double maskArea = 0.0;
    double sigArea = 0.0;

    for (int i = 0; i < span; ++i) {
        float mh = 0.0f;
        if (i < maskWidthSamples) {
            const float tNorm = static_cast<float>(i) * invMask;
            /* Mild trapezoid so rise/fall at edges are part of the mask */
            mh = maskHeightAt(tNorm, hPeak, hPeak, 0.12f, 0.12f);
        }

        float sh = 0.0f;
        const int si = onsetIdx + i;
        if (i < pulseW && si >= 0 && si < m_powerHist.size()) {
            sh = std::max(0.0f, m_powerHist[si] - baseDb);
        }

        overlap += static_cast<double>(std::min(sh, mh));
        maskArea += static_cast<double>(std::max(0.0f, mh));
        sigArea += static_cast<double>(std::max(0.0f, sh));
    }

    if (maskArea < 1e-3 || sigArea < 1e-3) {
        return 0.0f;
    }

    /* Dice: pulse and mask should occupy the same area */
    const float dice = static_cast<float>(2.0 * overlap / (maskArea + sigArea));
    /* Cover: fraction of mask filled by pulse energy */
    const float cover = static_cast<float>(overlap / maskArea);
    /* Containment: fraction of pulse energy that sits inside the mask */
    const float contain = static_cast<float>(overlap / sigArea);

    /*
     * Width alignment soft factor: pulse duration vs mask width.
     * Full-length match (rise through fall) is rewarded when lengths agree.
     */
    const float lenRatio = static_cast<float>(pulseW)
                           / static_cast<float>(std::max(1, maskWidthSamples));
    float lenFit = 1.0f;
    if (lenRatio < 1.0f) {
        /* Pulse shorter than mask — under-fills (already in cover); mild extra */
        lenFit = std::max(0.35f, lenRatio);
    } else if (lenRatio > 1.0f) {
        /* Pulse longer than mask — spills past right edge (already in contain) */
        lenFit = std::max(0.35f, 1.0f / lenRatio);
    }

    /* Weighted: fill mask, keep pulse inside mask, length agreement */
    return 0.40f * cover + 0.30f * contain + 0.20f * dice + 0.10f * lenFit;
}

int MaskMorseWindow::histIndexAtOrBefore(qint64 tMs) const
{
    int best = -1;
    for (int i = 0; i < m_timeHist.size(); ++i) {
        if (m_timeHist[i] <= tMs) {
            best = i;
        } else {
            break;
        }
    }
    return best;
}

bool MaskMorseWindow::inDigitContext() const
{
    if (m_digitGroupRemain > 0) {
        return true;
    }
    /* Building a digit: only dots (5/H) or only dashes so far (0/9/M/O/T) */
    if (m_pendingMorse.isEmpty()) {
        return false;
    }
    bool onlyDot = true;
    bool onlyDash = true;
    for (QChar c : m_pendingMorse) {
        if (c != QChar('.')) {
            onlyDot = false;
        }
        if (c != QChar('-')) {
            onlyDash = false;
        }
    }
    /* 5 = ..... ; 9/0 start with several dahs; after five dots expect 9 */
    if (onlyDot && m_pendingMorse.size() >= 3 && m_pendingMorse.size() <= 6) {
        return true;
    }
    if (onlyDash && m_pendingMorse.size() >= 1 && m_pendingMorse.size() <= 5) {
        return true;
    }
    /* After a committed 5, pending may be empty but digit group still open */
    if (!m_scrollText.isEmpty() && m_scrollText.endsWith(QChar('5'))) {
        return true;
    }
    return false;
}

void MaskMorseWindow::extendPulseByEnergy(int *onsetIdx, int *endIdx) const
{
    if (!onsetIdx || !endIdx || m_powerHist.isEmpty()) {
        return;
    }
    int lo = std::max(0, *onsetIdx);
    int hi = std::min(m_powerHist.size(), std::max(lo + 1, *endIdx));
    if (hi <= lo) {
        return;
    }

    float peak = m_powerHist[lo];
    for (int i = lo; i < hi; ++i) {
        peak = std::max(peak, m_powerHist[i]);
    }
    float floorDb = m_noiseFloorDb;
    if (m_localReady && m_localFloorDb > -180.0f) {
        floorDb = std::max(floorDb, m_localFloorDb);
    }
    if (floorDb < -180.0f) {
        floorDb = m_thresholdDb - 12.0f;
    }
    /*
     * Stay in "mark energy" while above the stronger of:
     *   noise+7 dB, thr−4 dB, peak−11 dB
     * so thr-tip-only islands expand to the full dit/dah envelope body.
     */
    const float gate = std::max(std::max(floorDb + 7.0f, m_thresholdDb - 4.0f),
                                peak - 11.0f);

    const double dtSec = estimateDtSec();
    const int maxExpand = std::max(6, static_cast<int>(std::lround(
                                          effectiveDahSec() * 1.35 / std::max(1e-4, dtSec))));

    int left = lo;
    for (int k = 0; k < maxExpand && left > 0; ++k) {
        if (m_powerHist[left - 1] >= gate) {
            --left;
        } else {
            break;
        }
    }
    int right = hi;
    for (int k = 0; k < maxExpand && right < m_powerHist.size(); ++k) {
        if (m_powerHist[right] >= gate) {
            ++right;
        } else {
            break;
        }
    }
    *onsetIdx = left;
    *endIdx = right;
}

void MaskMorseWindow::tryMatchPulse(int onsetIdx, int endIdx, bool allowLongSplit)
{
    if (m_matchedThisMark) {
        return;
    }
    if (onsetIdx < 0 || endIdx <= onsetIdx || onsetIdx >= m_powerHist.size()) {
        return;
    }
    endIdx = std::min(endIdx, m_powerHist.size());
    if (endIdx - onsetIdx < 3) {
        return;
    }

    /* Expand thr-high island using envelope energy (digit dah recovery). */
    extendPulseByEnergy(&onsetIdx, &endIdx);
    endIdx = std::min(endIdx, m_powerHist.size());
    if (endIdx - onsetIdx < 3) {
        return;
    }

    /* Prefer look-ahead local widths (pre-adjusted from ~4 dahs ahead) */
    const double ditSec = effectiveDitSec();
    const double dahSec = effectiveDahSec();
    const double dtSec = estimateDtSec();
    const double pulseSec = (endIdx - onsetIdx) * dtSec;
    const double unit = std::max(0.040, m_unitSeconds);
    const bool digitCtx = inDigitContext();

    /*
     * Safety net for glued O (---) only — never during digit runs (5/9/0/SK)
     * where long thr-high blobs are multi-element digits, not a single O.
     */
    const double dahLen = std::max(std::max(dahSec, unit * 3.0), 0.18);
    const bool longLikeO = pulseSec >= 0.75 || pulseSec >= dahLen * 2.4;
    if (allowLongSplit && longLikeO && !digitCtx && m_pendingMorse.isEmpty()) {
        int nDah = static_cast<int>(std::lround(pulseSec / dahLen));
        nDah = std::max(2, std::min(5, nDah));
        /* Classic glued O at ~8–15 WPM: ~0.9–1.3 s super-mark */
        if (pulseSec >= 0.85 && pulseSec <= 1.45) {
            nDah = 3;
        } else if (pulseSec >= dahLen * 2.6 && pulseSec <= dahLen * 3.6) {
            nDah = 3;
        }
        const double each = pulseSec / static_cast<double>(nDah);
        if (each >= unit * 1.5 && each >= 0.12) {
            float baseDb = pulseBaseDb(onsetIdx);
            float hPeak = pulsePeakHeight(onsetIdx, endIdx, baseDb);
            const float peakDb = baseDb + std::max(hPeak, 1.5f);
            const int total = endIdx - onsetIdx;
            for (int i = 0; i < nDah; ++i) {
                const int s0 = onsetIdx + (total * i) / nDah;
                const int s1 = onsetIdx + (total * (i + 1)) / nDah;
                noteMarkDuration(each);
                m_matchedThisMark = false;
                tryMatchPulse(s0, s1, false);
                if (!m_matchedThisMark) {
                    emitElement(Element::Dah, each, peakDb);
                    m_matchedThisMark = true;
                    m_lastDahScore = std::max(m_lastDahScore, 0.55f);
                }
            }
            m_matchedThisMark = true;
            recomputeDitDahWidths();
            if (!m_timeHist.isEmpty()) {
                const int tIdx = std::min(m_timeHist.size() - 1, endIdx - 1);
                m_lastElementEndMs = m_timeHist[std::max(0, tIdx)];
            }
            /* Do not force letter flush — pending may still be building O/0/9. */
            return;
        }
    }

    const int ditW = std::max(4, static_cast<int>(std::lround(ditSec / dtSec)));
    const int dahW = std::max(ditW + 2, static_cast<int>(std::lround(dahSec / dtSec)));

    float baseDb = pulseBaseDb(onsetIdx);
    if (m_localReady && m_localFloorDb > -180.0f) {
        baseDb = 0.5f * baseDb + 0.5f * m_localFloorDb;
    }
    float hPeak = pulsePeakHeight(onsetIdx, endIdx, baseDb);
    /* Local look-ahead peak sets mask height scale when stronger */
    if (m_localReady && m_localPeakDb > -180.0f) {
        const float localH = std::max(0.0f, m_localPeakDb - baseDb);
        if (localH > 1.0f) {
            hPeak = std::max(hPeak, 0.65f * hPeak + 0.35f * localH);
        }
    }
    if (hPeak < 1.5f) {
        m_lastDitScore = 0.0f;
        m_lastDahScore = 0.0f;
        return;
    }

    const float ditScore = scorePulseFillMask(onsetIdx, endIdx, ditW, baseDb, hPeak);
    const float dahScore = scorePulseFillMask(onsetIdx, endIdx, dahW, baseDb, hPeak);

    m_lastDitScore = ditScore;
    m_lastDahScore = dahScore;

    /*
     * Duration gate (captures show dits ~90–160 ms, dahs ~290–490 ms):
     * use fill scores with lower dit threshold; prefer duration when scores close.
     */
    const double midSplit = 0.5 * (ditSec + dahSec); /* ~2× dit when 3:1 */
    const bool durationDit = pulseSec < midSplit;
    const bool durationDah = pulseSec >= midSplit;

    Element best = Element::None;

    const bool ditOk = ditScore >= kMinOverlayDit;
    const bool dahOk = dahScore >= kMinOverlayDah;

    if (ditOk && dahOk) {
        if (durationDit && ditScore + 0.02f >= dahScore) {
            best = Element::Dit;
        } else if (durationDah && dahScore + 0.02f >= ditScore) {
            best = Element::Dah;
        } else if (dahScore > ditScore + 0.05f) {
            best = Element::Dah;
        } else if (ditScore > dahScore + 0.05f) {
            best = Element::Dit;
        } else {
            best = durationDit ? Element::Dit : Element::Dah;
        }
    } else if (ditOk) {
        /* Short pulse that fills dit — accept even if slightly long */
        if (pulseSec < dahSec * 0.72) {
            best = Element::Dit;
        } else if (dahScore >= kMinOverlayDit) {
            best = Element::Dah;
        } else {
            best = Element::Dit;
        }
    } else if (dahOk) {
        best = Element::Dah;
    } else {
        /*
         * Weak fill (common on thin dits): fall back to pure duration vs
         * local look-ahead widths so we do not drop most dits.
         */
        if (pulseSec >= 0.028 && pulseSec < midSplit && pulseSec <= ditSec * 1.85) {
            best = Element::Dit;
            m_lastDitScore = std::max(m_lastDitScore, 0.40f);
        } else if (pulseSec >= midSplit && pulseSec <= dahSec * 1.6) {
            best = Element::Dah;
            m_lastDahScore = std::max(m_lastDahScore, 0.40f);
        }
    }

    /*
     * Clipped-dah recovery (59N: thr only tags dah tips → --... instead of ----.).
     * In digit context, a short but strong pulse that nearly fills the dah mask
     * is more likely a thr-chopped dah than a true dit.
     */
    const float peakAbs = baseDb + hPeak;
    const bool strongPeak = (peakAbs >= m_thresholdDb + 3.0f)
                            || (m_localReady && m_localPeakDb > -180.0f
                                && peakAbs >= m_localPeakDb - 6.0f);
    if (best == Element::Dit && digitCtx && strongPeak
        && pulseSec >= unit * 0.45 && pulseSec <= dahSec * 1.15) {
        if (dahScore + 0.08f >= ditScore || pulseSec >= ditSec * 0.85) {
            best = Element::Dah;
            m_lastDahScore = std::max(m_lastDahScore, std::max(dahScore, 0.42f));
        }
    }
    /* Building 9/0: pending is only dashes — short strong marks stay dahs */
    if (best == Element::Dit && strongPeak && !m_pendingMorse.isEmpty()) {
        bool onlyDash = true;
        for (QChar c : m_pendingMorse) {
            if (c != QChar('-')) {
                onlyDash = false;
                break;
            }
        }
        if (onlyDash && m_pendingMorse.size() <= 4
            && pulseSec >= unit * 0.50 && pulseSec < midSplit
            && dahScore + 0.12f >= ditScore) {
            best = Element::Dah;
            m_lastDahScore = std::max(m_lastDahScore, 0.42f);
        }
    }

    if (best == Element::None) {
        return;
    }

    /*
     * Glitch strip: sub-dit thr spikes (not strong) must not enter pending —
     * turns 5 into ...... and wrecks letter assembly.
     */
    if (best == Element::Dit && pulseSec < unit * 0.35 && !strongPeak) {
        return;
    }
    if (pulseSec < 0.022) {
        return;
    }

    emitElement(best, pulseSec, peakAbs);
    m_matchedThisMark = true;
    if (!m_timeHist.isEmpty()) {
        const int tIdx = std::min(m_timeHist.size() - 1, endIdx - 1);
        m_lastElementEndMs = m_timeHist[std::max(0, tIdx)];
    }
}

void MaskMorseWindow::processIntraMarkValley(float powerDb, qint64 nowMs)
{
    if (!m_keyHigh) {
        m_inValley = false;
        m_valleyStartMs = -1;
        return;
    }

    const double unit = std::max(0.045, m_unitSeconds);
    /*
     * Valley = amplitude drop between dahs of O while Schmitt key stays high.
     * cwtrace_20260801_155432: O valleys go to −70 while peaks ~−53 (thr ~−54).
     */
    if (powerDb >= m_thresholdDb) {
        m_valleyNeedsMarkPeak = false;
    }

    const float peakRef = std::max(m_segPeakDb, m_markPeakDb);
    const float dropDb = 5.0f;
    const bool deepVsPeak = (peakRef > -180.0f) && (powerDb <= peakRef - dropDb);
    const bool softVsThr = powerDb < (m_thresholdDb + 1.5f);
    const bool inValleyNow = deepVsPeak && softVsThr && !m_valleyNeedsMarkPeak;

    /*
     * Near-dah body before valley-splitting. On OA capture thr sticks high
     * over a long soft stretch — long-mark safety net (full ~1.1 s → 3 dahs)
     * is preferred unless valleys cleanly separate dah-length peaks.
     */
    const double minBody = m_unitCalibrated ? std::max(0.22, unit * 2.4)
                                            : 0.28;
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
            const double bodyToValley = (m_valleyStartMs - m_markStartMs) / 1000.0;
            if (vSec >= minValley && bodyToValley >= minBody) {
                splitMarkAtValley(m_valleyStartMs, nowMs);
            } else if (vSec > maxValley && bodyToValley >= minBody) {
                splitMarkAtValley(m_valleyStartMs, nowMs);
            } else if (vSec > maxValley && bodyToValley < minBody) {
                /*
                 * Soft stretch is not an element gap. Drop valley tracking and
                 * require a thr-level peak before another valley attempt so we
                 * do not nibble the glued O into partial dahs.
                 */
                m_inValley = false;
                m_valleyStartMs = -1;
                m_valleyNeedsMarkPeak = true;
            }
        }
    } else if (m_inValley) {
        /*
         * Leave valley only when power is back at/above thr (real mark top).
         * Soft samples that merely fail deepVsPeak must not restart onset.
         */
        if (powerDb >= m_thresholdDb) {
            m_inValley = false;
            m_valleyStartMs = -1;
            m_markStartMs = nowMs;
            m_onsetHistIdx = m_powerHist.size() - 1;
            m_matchedThisMark = false;
            m_segPeakDb = powerDb;
            m_markPeakDb = powerDb;
            m_looking = true;
            m_ditHit = false;
            m_dahHit = false;
        }
    }
}

void MaskMorseWindow::splitMarkAtValley(qint64 valleyStartMs, qint64 nowMs)
{
    if (valleyStartMs <= m_markStartMs || m_onsetHistIdx < 0) {
        m_inValley = false;
        m_valleyStartMs = -1;
        return;
    }

    /* Exclusive end = first sample at/after valley onset */
    int endIdx = 0;
    for (int i = 0; i < m_timeHist.size(); ++i) {
        if (m_timeHist[i] < valleyStartMs) {
            endIdx = i + 1;
        } else {
            break;
        }
    }
    if (endIdx <= m_onsetHistIdx) {
        endIdx = histIndexAtOrBefore(valleyStartMs - 1) + 1;
    }
    endIdx = std::min(endIdx, m_powerHist.size());

    const double markSec = (valleyStartMs - m_markStartMs) / 1000.0;
    if (endIdx - m_onsetHistIdx >= 3 && !m_matchedThisMark) {
        noteMarkDuration(markSec);
        recomputeDitDahWidths();
        updateLocalMaskFromLookahead();
        /* Segment is already one element — do not re-split into 3 dahs */
        tryMatchPulse(m_onsetHistIdx, endIdx, false);
    }

    /* Resume as still key-high after the valley for the next element */
    m_inValley = false;
    m_valleyStartMs = -1;
    m_valleyNeedsMarkPeak = true; /* next valley only after a thr-level peak */
    m_markStartMs = nowMs;
    m_onsetHistIdx = m_powerHist.size() - 1;
    m_matchedThisMark = false;
    m_segPeakDb = m_lastPowerDb;
    m_markPeakDb = m_lastPowerDb;
    m_looking = true;
    m_ditHit = false;
    m_dahHit = false;
}

void MaskMorseWindow::emitElement(Element el, double durationSec, float peakDb)
{
    if (el == Element::None) {
        return;
    }
    /* Mark durations for width measurement come from thr key-high timing
     * (noteMarkDuration at mark end). Here we only assemble the letter. */
    (void)durationSec;

    const QChar sym = (el == Element::Dit) ? QChar('.') : QChar('-');
    m_pendingMorse.append(sym);
    m_markPeakDb = peakDb;
    if (m_peakHoldDb < -180.0f || peakDb > m_peakHoldDb) {
        m_peakHoldDb = peakDb;
    }

    m_ditHit = (el == Element::Dit);
    m_dahHit = (el == Element::Dah);
    m_looking = false;
    m_hitHoldUntilMs = m_clock.elapsed() + 350;

    /* Allow SK / ? patterns (6 elements); strip only runaway garbage */
    if (m_pendingMorse.size() > 10) {
        m_pendingMorse.clear();
    }
    updateUi();
}

void MaskMorseWindow::publishMaskOverlay()
{
    const qint64 now = m_clock.elapsed();
    if (m_hitHoldUntilMs > 0 && now > m_hitHoldUntilMs) {
        m_ditHit = false;
        m_dahHit = false;
        m_looking = true;
        m_hitHoldUntilMs = 0;
    }

    float floorDb = m_localReady ? m_localFloorDb : m_noiseFloorDb;
    if (floorDb < -180.0f) {
        floorDb = m_noiseFloorDb;
    }
    if (floorDb < -180.0f) {
        floorDb = m_thresholdDb - 12.0f;
    }

    float peak = effectivePeakDb();
    if (peak < floorDb + 1.5f) {
        peak = floorDb + 1.5f;
    }
    emit maskOverlayChanged(effectiveDitSec(), effectiveDahSec(), floorDb, peak,
                            m_lastDitScore, m_lastDahScore,
                            m_looking && !m_ditHit && !m_dahHit,
                            m_ditHit, m_dahHit);
}

void MaskMorseWindow::noteLetterGap(double gapSeconds)
{
    if (gapSeconds < 0.12 || gapSeconds > 4.0) {
        return;
    }
    m_letterGaps.append(gapSeconds);
    while (m_letterGaps.size() > kMaxLetterGaps) {
        m_letterGaps.removeFirst();
    }
}

double MaskMorseWindow::wordMinSeconds() const
{
    const double unit = std::max(0.050, m_unitSeconds);
    if (!m_unitCalibrated) {
        return 1.20;
    }
    /*
     * Word spaces are significant. Many fists use ~3.5–4.5× dit for words
     * (cwtrace_20260807_090453 buried "QM TU GA JE": gaps ~240–270 ms at
     * dit~65 ms). A 5.5–7× floor erased those spaces → "QMTUGAJE".
     */
    double wordMin = unit * 4.0;
    if (m_letterGaps.size() >= 2) {
        const double medGap = medianOf(m_letterGaps);
        wordMin = std::max(unit * 3.5, medGap * 1.20);
        wordMin = std::min(wordMin, unit * 10.0);
    }
    wordMin = std::max(wordMin, unit * 2.80);
    return wordMin;
}

void MaskMorseWindow::handleSpaceGap(double gapSec)
{
    if (m_pendingMorse.isEmpty()) {
        return;
    }
    /*
     * Grouping:
     *   element gap ≈ 1 dit  → stay in letter  (cut at 2.20× unit)
     *   letter gap  ≈ 3 dits → commit letter
     *   word gap    ≈ 3.5–4.5 dits (fist) / up to ~7 ITU → insert space
     * Digit tokens (59N): after '5', suppress word space for next 2 letters
     * unless gap is clearly large (≳ 6× dit).
     */
    /* Floor unit so under-calibrated dit (glitches) cannot make every gap a letter */
    const double unit = std::max(0.050, m_unitSeconds);
    const double elemMax = m_unitCalibrated ? (unit * 2.20) : 0.80;
    if (gapSec < elemMax) {
        return; /* element gap inside letter */
    }

    const char ch = morseToChar(m_pendingMorse);
    if (ch != 0) {
        appendChar(QChar(ch));
    } else if (m_pendingMorse.size() >= 1) {
        appendChar(QChar('?'));
    }
    m_pendingMorse.clear();

    const double wordMin = wordMinSeconds();
    const double hardWord = unit * 6.0; /* always a word, even mid-digit-group */
    bool insertWord = (gapSec >= wordMin);
    if (insertWord && m_digitGroupRemain > 0 && gapSec < hardWord) {
        /* Keep 5-9-N as one spaced token: "59N" not "5 9 N" */
        insertWord = false;
        noteLetterGap(gapSec);
        --m_digitGroupRemain;
    } else if (insertWord) {
        if (!m_scrollText.isEmpty() && !m_scrollText.endsWith(QChar(' '))) {
            appendChar(QChar(' '));
        }
        m_digitGroupRemain = 0;
    } else {
        noteLetterGap(gapSec);
        if (m_digitGroupRemain > 0) {
            --m_digitGroupRemain;
        }
    }
}

void MaskMorseWindow::flushLetterIfNeeded(qint64 nowMs)
{
    if (m_pendingMorse.isEmpty() || m_lastElementEndMs < 0) {
        return;
    }
    const double gap = (nowMs - m_lastElementEndMs) / 1000.0;
    const double unit = std::max(0.050, m_unitSeconds);
    /* Letter timeout above elemMax so element gaps never flush mid-letter. */
    const double letterMin = m_unitCalibrated ? (unit * 2.60) : 0.90;
    if (gap >= letterMin) {
        handleSpaceGap(gap);
    }
}

void MaskMorseWindow::feedSample(float powerDb, bool aboveThreshold, qint64 sampleTimeMs)
{
    const qint64 nowMs = (sampleTimeMs >= 0) ? sampleTimeMs : m_clock.elapsed();
    m_lastPowerDb = powerDb;

    m_powerHist.append(powerDb);
    m_timeHist.append(nowMs);
    while (m_powerHist.size() > kHistMax) {
        /* Keep onset index valid when history slides left */
        if (m_onsetHistIdx >= 0) {
            m_onsetHistIdx = std::max(-1, m_onsetHistIdx - 1);
        }
        m_powerHist.removeFirst();
        m_timeHist.removeFirst();
    }

    /* Pre-adjust mask from look-ahead (newest ~4 dahs) before matching */
    updateLocalMaskFromLookahead();

    /* Track envelope peak for mask top; decay quickly so weaker signals shrink height. */
    if (powerDb > m_peakHoldDb || m_peakHoldDb < -180.0f) {
        m_peakHoldDb = powerDb;
    } else {
        m_peakHoldDb -= 0.25f; /* fall with signal strength (~0.2–0.4 s) */
    }

    if (!m_hasState) {
        m_hasState = true;
        m_keyHigh = aboveThreshold;
        m_lastSampleMs = nowMs;
        m_looking = true;
        m_inValley = false;
        m_valleyStartMs = -1;
        m_valleyNeedsMarkPeak = false;
        if (aboveThreshold) {
            m_markStartMs = nowMs;
            m_onsetHistIdx = m_powerHist.size() - 1;
            m_matchedThisMark = false;
            m_markPeakDb = powerDb;
            m_segPeakDb = powerDb;
            m_looking = true;
            m_ditHit = false;
            m_dahHit = false;
        }
        updateUi();
        return;
    }

    /* Letter timeout while key up */
    if (!aboveThreshold && !m_keyHigh) {
        flushLetterIfNeeded(nowMs);
    }

    if (aboveThreshold != m_keyHigh) {
        if (aboveThreshold) {
            /* Mark onset — dit and dah masks share this start (red = looking) */
            m_markStartMs = nowMs;
            m_onsetHistIdx = m_powerHist.size() - 1;
            m_matchedThisMark = false;
            m_markPeakDb = powerDb;
            m_segPeakDb = powerDb;
            m_inValley = false;
            m_valleyStartMs = -1;
            m_valleyNeedsMarkPeak = false;
            m_looking = true;
            m_ditHit = false;
            m_dahHit = false;

            /* Close previous letter if long enough space */
            if (m_lastElementEndMs >= 0) {
                const double gap = (nowMs - m_lastElementEndMs) / 1000.0;
                handleSpaceGap(gap);
            }
        } else {
            /*
             * Falling edge — full pulse is now known (rise → fall).
             * If mid-valley, end body at valley start (not soft-dip tail).
             * Threshold only delimits the energy blob; classification is
             * how completely that blob fills the dit vs dah mask.
             */
            qint64 markEndMs = nowMs;
            if (m_inValley && m_valleyStartMs > m_markStartMs) {
                markEndMs = m_valleyStartMs;
            }
            const double markDur = std::max(0.0005, (markEndMs - m_markStartMs) / 1000.0);
            noteMarkDuration(markDur);
            recomputeDitDahWidths();
            updateLocalMaskFromLookahead();

            int endIdx = m_powerHist.size();
            if (markEndMs < nowMs) {
                const int hi = histIndexAtOrBefore(markEndMs);
                if (hi >= 0) {
                    endIdx = hi + 1;
                }
            }
            if (!m_matchedThisMark && m_onsetHistIdx >= 0) {
                tryMatchPulse(m_onsetHistIdx, endIdx, true);
            }
            if (m_lastElementEndMs < 0) {
                m_lastElementEndMs = nowMs;
            } else if (m_matchedThisMark) {
                m_lastElementEndMs = nowMs;
            }
            m_inValley = false;
            m_valleyStartMs = -1;
            m_valleyNeedsMarkPeak = false;
            flushLetterIfNeeded(nowMs + static_cast<qint64>(m_unitSeconds * 2500));
        }
        m_keyHigh = aboveThreshold;
    } else if (aboveThreshold) {
        m_markPeakDb = std::max(m_markPeakDb, powerDb);
        m_segPeakDb = std::max(m_segPeakDb, powerDb);
        /* Split glued multi-dah letters (O=---) when thr key never drops */
        processIntraMarkValley(powerDb, nowMs);
        /*
         * Live preview scores only (no commit): how the open pulse is filling
         * the masks so far. Final decision waits for falling edge (or valley).
         */
        if (!m_matchedThisMark && m_onsetHistIdx >= 0) {
            const int endIdx = m_powerHist.size();
            if (endIdx - m_onsetHistIdx >= 4) {
                const float baseDb = pulseBaseDb(m_onsetHistIdx);
                const float hPeak = pulsePeakHeight(m_onsetHistIdx, endIdx, baseDb);
                if (hPeak >= 1.5f) {
                    const double dtSec = estimateDtSec();
                    const int ditW = std::max(
                        4, static_cast<int>(std::lround(effectiveDitSec() / dtSec)));
                    const int dahW = std::max(
                        ditW + 2,
                        static_cast<int>(std::lround(effectiveDahSec() / dtSec)));
                    m_lastDitScore =
                        scorePulseFillMask(m_onsetHistIdx, endIdx, ditW, baseDb, hPeak);
                    m_lastDahScore =
                        scorePulseFillMask(m_onsetHistIdx, endIdx, dahW, baseDb, hPeak);
                }
            }
        }
    }

    m_lastSampleMs = nowMs;
    updateUi();
}

void MaskMorseWindow::noteMarkDuration(double markSeconds)
{
    /*
     * Threshold key-high mark lengths only.
     * Cap long stuck-key runs so they do not dominate clustering
     * (capture analysis: good dits ~60–100 ms, dahs ~250–360 ms).
     */
    /*
     * Ignore thr glitches (<35 ms) so unit does not collapse and turn every
     * real element gap into a letter break (T/E spam). Cap long stuck keys.
     */
    if (markSeconds < 0.035 || markSeconds > 0.90) {
        return;
    }
    m_allMarkSecs.append(markSeconds);
    while (m_allMarkSecs.size() > kMaxMarkHistory) {
        m_allMarkSecs.removeFirst();
    }
}

void MaskMorseWindow::recomputeDitDahWidths()
{
    /*
     * Cluster threshold key-high mark durations into short (dit) and long (dah).
     * Capture cwtrace_20260801_151904: proper dit≈90 ms, dah≈290 ms (~13 WPM).
     * Bad 273/678 was mono-cluster of dahs treated as dits then ×2.5.
     */
    QVector<double> marks;
    for (double d : m_allMarkSecs) {
        if (d >= 0.035 && d <= 0.90) {
            marks.append(d);
        }
    }
    if (marks.isEmpty()) {
        return;
    }
    std::sort(marks.begin(), marks.end());

    double ditEst = 0.0;
    double dahEst = 0.0;

    if (marks.size() == 1) {
        if (marks[0] >= 0.14) {
            dahEst = marks[0];
            ditEst = marks[0] / 3.0;
        } else {
            ditEst = marks[0];
            dahEst = 3.0 * marks[0];
        }
    } else {
        /*
         * Primary: percentile cut (robust on mixed fists).
         * Short group = marks below ~1.7× the 30th percentile.
         */
        const int pIdx = std::max(0, static_cast<int>(0.30 * (marks.size() - 1)));
        const double p30 = marks[pIdx];
        const double cut = p30 * 1.70;
        QVector<double> shorts;
        QVector<double> longs;
        for (double d : marks) {
            if (d < cut) {
                shorts.append(d);
            } else {
                longs.append(d);
            }
        }

        /* Secondary: largest gap with 1.8–4.5× ratio (classic dit/dah split) */
        int bestSplit = -1;
        double bestScore = -1.0;
        for (int i = 1; i < marks.size(); ++i) {
            const double gap = marks[i] - marks[i - 1];
            const double ratio = marks[i] / std::max(1e-4, marks[i - 1]);
            if (ratio < 1.75 || ratio > 5.0) {
                continue;
            }
            const double score = gap * ratio;
            if (score > bestScore) {
                bestScore = score;
                bestSplit = i;
            }
        }
        if (bestSplit > 0 && bestScore > 0.02) {
            const double midCut = 0.5 * (marks[bestSplit - 1] + marks[bestSplit]);
            shorts.clear();
            longs.clear();
            for (double d : marks) {
                if (d <= midCut) {
                    shorts.append(d);
                } else {
                    longs.append(d);
                }
            }
        }

        if (!shorts.isEmpty() && !longs.isEmpty()) {
            ditEst = medianOf(shorts);
            dahEst = medianOf(longs);
            const double ratio = dahEst / std::max(1e-4, ditEst);
            /*
             * Weak split (both clusters look like dahs): e.g. 250 vs 320 ms.
             * Do NOT invent dah = 2.5×dit (that produced 273/678). Treat as
             * mono-dah and recover dit = dah/3.
             */
            if (ratio < 2.0 || ditEst >= 0.14) {
                const double body = medianOf(marks);
                if (body >= 0.14) {
                    dahEst = body;
                    ditEst = body / 3.0;
                } else {
                    ditEst = body;
                    dahEst = 3.0 * body;
                }
            }
        } else if (!longs.isEmpty()) {
            dahEst = medianOf(longs);
            ditEst = dahEst / 3.0;
        } else {
            /* Only "shorts" — may be all dits or all dahs */
            const double body = medianOf(shorts.isEmpty() ? marks : shorts);
            if (body >= 0.14) {
                dahEst = body;
                ditEst = body / 3.0;
            } else {
                ditEst = body;
                dahEst = 3.0 * body;
            }
        }
    }

    /* Sanity clamps from capture analysis + QRS/QRQ range */
    ditEst = std::max(0.030, std::min(0.22, ditEst));
    dahEst = std::max(ditEst * 2.2, std::min(0.75, dahEst));
    /* Prefer ~3× when ratio drifted */
    if (dahEst / ditEst < 2.4 || dahEst / ditEst > 4.2) {
        const double body = medianOf(marks);
        if (body >= 0.16) {
            dahEst = std::max(dahEst, body);
            ditEst = dahEst / 3.0;
            ditEst = std::max(0.030, std::min(0.22, ditEst));
        } else {
            dahEst = 3.0 * ditEst;
        }
    }

    m_measDitSec = ditEst;
    m_measDahSec = dahEst;

    if (m_autoWidth) {
        const double a = (m_allMarkSecs.size() < 4) ? 0.50 : 0.22;
        m_ditWidthSec = (1.0 - a) * m_ditWidthSec + a * ditEst;
        m_dahWidthSec = (1.0 - a) * m_dahWidthSec + a * dahEst;
        m_ditWidthSec = std::max(0.030, std::min(0.22, m_ditWidthSec));
        m_dahWidthSec = std::max(m_ditWidthSec * 2.2, std::min(0.75, m_dahWidthSec));
        m_unitSeconds = m_ditWidthSec;
        m_unitCalibrated = true;
        syncWidthSlidersFromEffective();
    }

    if (m_autoWpm) {
        m_unitSeconds = m_ditWidthSec;
        m_unitCalibrated = true;
        if (m_wpmSlider && m_ditWidthSec > 1e-4) {
            const int tenths = static_cast<int>(std::lround((1.2 / m_ditWidthSec) * 10.0));
            const int clamped = std::max(m_wpmSlider->minimum(),
                                         std::min(m_wpmSlider->maximum(), tenths));
            if (std::abs(clamped - m_wpmSlider->value()) >= 1) {
                m_wpmSlider->blockSignals(true);
                m_wpmSlider->setValue(clamped);
                m_wpmSlider->blockSignals(false);
                if (m_wpmValueLabel) {
                    m_wpmValueLabel->setText(
                        QStringLiteral("%1 WPM").arg(clamped / 10.0, 0, 'f', 1));
                }
            }
        }
    }
}

void MaskMorseWindow::recomputeUnitFromMarks()
{
    /* Legacy entry — full dit/dah measurement path. */
    recomputeDitDahWidths();
}

void MaskMorseWindow::appendChar(QChar ch)
{
    if (ch == QChar('5')) {
        /* Expect 9 then N (or similar) as same word group */
        m_digitGroupRemain = 2;
    } else if (ch == QChar(' ') || ch == QChar('?')) {
        m_digitGroupRemain = 0;
    }
    m_scrollText.append(ch);
    trimScrollBuffer();
    refreshTextView();
    emit textChanged(m_scrollText);
}

void MaskMorseWindow::trimScrollBuffer()
{
    if (m_scrollText.size() > kMaxScrollChars) {
        m_scrollText = m_scrollText.right(kMaxScrollChars * 3 / 4);
    }
}

void MaskMorseWindow::refreshTextView()
{
    if (!m_textView) {
        return;
    }
    m_textView->setPlainText(m_scrollText);
    QTextCursor c = m_textView->textCursor();
    c.movePosition(QTextCursor::End);
    m_textView->setTextCursor(c);
}

void MaskMorseWindow::clearDecode()
{
    m_scrollText.clear();
    m_pendingMorse.clear();
    m_matchedThisMark = false;
    m_lastDitScore = 0.0f;
    m_lastDahScore = 0.0f;
    m_digitGroupRemain = 0;
    refreshTextView();
    emit textChanged(m_scrollText);
    updateUi();
}

void MaskMorseWindow::resetTiming()
{
    applyWpmFromSlider();
    m_unitSeconds = m_sliderUnitSeconds;
    if (m_autoWidth || !m_ditWidthSlider) {
        m_ditWidthSec = m_unitSeconds;
        m_dahWidthSec = 3.0 * m_unitSeconds;
        m_measDitSec = m_ditWidthSec;
        m_measDahSec = m_dahWidthSec;
        syncWidthSlidersFromEffective();
    } else {
        applyWidthSliders();
    }
    m_localDitSec = m_ditWidthSec;
    m_localDahSec = m_dahWidthSec;
    m_localPeakDb = -200.0f;
    m_localFloorDb = m_noiseFloorDb;
    m_localReady = false;
    m_unitCalibrated = !m_autoWpm || !m_autoWidth;
    m_allMarkSecs.clear();
    m_letterGaps.clear();
    m_powerHist.clear();
    m_timeHist.clear();
    m_hasState = false;
    m_keyHigh = false;
    m_matchedThisMark = false;
    m_onsetHistIdx = -1;
    m_lastElementEndMs = -1;
    m_pendingMorse.clear();
    m_peakHoldDb = -200.0f;
    m_lastPowerDb = -200.0f;
    m_segPeakDb = -200.0f;
    m_inValley = false;
    m_valleyStartMs = -1;
    m_valleyNeedsMarkPeak = false;
    m_digitGroupRemain = 0;
    updateUi();
}

void MaskMorseWindow::prepareForPlayback()
{
    clearDecode();
    resetTiming();
    m_clock.restart();
}

void MaskMorseWindow::setPlaybackMode(bool /*on*/)
{
    /* Mask path does not use thr squelch; always open */
}

void MaskMorseWindow::onClearClicked()
{
    clearDecode();
}

void MaskMorseWindow::onResetTimingClicked()
{
    resetTiming();
}

void MaskMorseWindow::onWpmSliderChanged(int /*wpmTenths*/)
{
    /* Manual WPM seed — does not override independent width sliders unless
     * Auto widths is on and we still lack measurements. */
    if (m_autoWpm && m_autoWpmCheck) {
        m_autoWpmCheck->blockSignals(true);
        m_autoWpmCheck->setChecked(false);
        m_autoWpmCheck->blockSignals(false);
        m_autoWpm = false;
    }
    applyWpmFromSlider();
    if (m_autoWidth && m_allMarkSecs.size() < 2) {
        m_ditWidthSec = m_sliderUnitSeconds;
        m_dahWidthSec = 3.0 * m_sliderUnitSeconds;
        syncWidthSlidersFromEffective();
    }
    updateUi();
}

void MaskMorseWindow::onAutoToggled(bool checked)
{
    m_autoWpm = checked;
    applyWpmFromSlider();
    updateUi();
}

void MaskMorseWindow::onAutoWidthToggled(bool checked)
{
    m_autoWidth = checked;
    if (m_autoWidth) {
        recomputeDitDahWidths();
        if (m_allMarkSecs.size() < 2) {
            m_ditWidthSec = std::max(0.020, m_unitSeconds);
            m_dahWidthSec = 3.0 * m_ditWidthSec;
            m_measDitSec = m_ditWidthSec;
            m_measDahSec = m_dahWidthSec;
            syncWidthSlidersFromEffective();
        }
    } else {
        applyWidthSliders();
    }
    updateUi();
}

void MaskMorseWindow::onDitWidthSliderChanged(int /*ms*/)
{
    if (m_autoWidth && m_autoWidthCheck) {
        m_autoWidthCheck->blockSignals(true);
        m_autoWidthCheck->setChecked(false);
        m_autoWidthCheck->blockSignals(false);
        m_autoWidth = false;
    }
    applyWidthSliders();
    updateUi();
}

void MaskMorseWindow::onDahWidthSliderChanged(int /*ms*/)
{
    if (m_autoWidth && m_autoWidthCheck) {
        m_autoWidthCheck->blockSignals(true);
        m_autoWidthCheck->setChecked(false);
        m_autoWidthCheck->blockSignals(false);
        m_autoWidth = false;
    }
    applyWidthSliders();
    updateUi();
}

void MaskMorseWindow::updateUi()
{
    /* Scope overlay only — no status/Pending text that jumps the layout. */
    publishMaskOverlay();

    if (m_headless) {
        return;
    }
    /* Keep WPM readout in sync; width labels update from slider handlers. */
    if (m_wpmValueLabel) {
        m_wpmValueLabel->setText(QStringLiteral("%1 WPM").arg(wpmF(), 0, 'f', 1));
    }
}
