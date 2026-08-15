/**
 * @file signaltracewindow.cpp
 * @brief Oscilloscope-style single-signal power vs time
 */

#include "signaltracewindow.h"

#include "spectrumwidget.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPolygonF>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

/* ------------------------------------------------------------------ */
/* SignalTracePlot                                                     */
/* ------------------------------------------------------------------ */

namespace {

void computeDisplayRange(const QVector<float> &samples, float *minDb, float *maxDb)
{
    if (samples.isEmpty()) {
        *minDb = -120.0f;
        *maxDb = -40.0f;
        return;
    }

    float lo = samples.first();
    float hi = samples.first();
    for (float v : samples) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }

    float span = hi - lo;
    if (span < 24.0f) {
        const float mid = 0.5f * (hi + lo);
        lo = mid - 12.0f;
        hi = mid + 12.0f;
        span = 24.0f;
    }

    const float pad = std::max(3.0f, span * 0.08f);
    *minDb = lo - pad;
    *maxDb = hi + pad;
}

}  // namespace

SignalTracePlot::SignalTracePlot(QWidget *parent)
    : QWidget(parent)
    , m_maxSamples(600)
    , m_spanSeconds(10)
    , m_noiseFloorDb(-95.0f)
    , m_thresholdDb(-80.0f)
    , m_markPeakDb(-200.0f)
    , m_showThreshold(true)
    , m_frozen(false)
    , m_displayMinDb(-120.0f)
    , m_displayMaxDb(-40.0f)
    , m_frozenMinDb(-120.0f)
    , m_frozenMaxDb(-40.0f)
    , m_maskEnabled(false)
    , m_maskDitSec(0.08)
    , m_maskDahSec(0.24)
    , m_maskFloorDb(-90.0f)
    , m_maskPeakDb(-50.0f)
    , m_maskDitScore(0.0f)
    , m_maskDahScore(0.0f)
    , m_maskLooking(true)
    , m_maskDitHit(false)
    , m_maskDahHit(false)
{
    setMinimumSize(480, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(10, 10, 12));
    setPalette(pal);
}

const QVector<float> &SignalTracePlot::displaySamples() const
{
    return m_frozen ? m_frozenSamples : m_samples;
}

void SignalTracePlot::setMaxSamples(int n)
{
    m_maxSamples = std::max(50, n);
    /* Never trim or alter the frozen snapshot */
    if (!m_frozen) {
        trimToMax();
    }
    update();
}

void SignalTracePlot::setSpanSeconds(int seconds)
{
    m_spanSeconds = std::max(1, seconds);
    update();
}

void SignalTracePlot::clear()
{
    if (m_frozen) {
        return;
    }
    m_samples.clear();
    m_frozenSamples.clear();
    m_displayMinDb = -120.0f;
    m_displayMaxDb = -40.0f;
    update();
}

void SignalTracePlot::append(float powerDb)
{
    if (m_frozen) {
        return;
    }
    if (!std::isfinite(powerDb)) {
        return;
    }
    m_samples.append(powerDb);
    trimToMax();

    float targetMin = m_displayMinDb;
    float targetMax = m_displayMaxDb;
    computeDisplayRange(m_samples, &targetMin, &targetMax);
    m_displayMinDb = 0.80f * m_displayMinDb + 0.20f * targetMin;
    m_displayMaxDb = 0.80f * m_displayMaxDb + 0.20f * targetMax;
    update();
}

void SignalTracePlot::seed(const QVector<float> &powerDbSamples)
{
    if (m_frozen) {
        return;
    }
    m_samples = powerDbSamples;
    trimToMax();
    computeDisplayRange(m_samples, &m_displayMinDb, &m_displayMaxDb);
    update();
}

void SignalTracePlot::setNoiseFloor(float noiseFloorDb)
{
    if (std::isfinite(noiseFloorDb) && noiseFloorDb > -180.0f) {
        m_noiseFloorDb = noiseFloorDb;
        if (!m_frozen) {
            update();
        }
    }
}

void SignalTracePlot::setFrozen(bool frozen)
{
    if (frozen && !m_frozen) {
        /* Take an immutable snapshot for hand decoding */
        m_frozenSamples = m_samples;
        m_frozenMinDb = m_displayMinDb;
        m_frozenMaxDb = m_displayMaxDb;
    } else if (!frozen && m_frozen) {
        /* Resume live from the snapshot (keep what user was looking at) */
        m_samples = m_frozenSamples;
        m_displayMinDb = m_frozenMinDb;
        m_displayMaxDb = m_frozenMaxDb;
        m_frozenSamples.clear();
    }
    m_frozen = frozen;
    update();
}

void SignalTracePlot::setThresholdDb(float thresholdDb)
{
    m_thresholdDb = thresholdDb;
    update();
}

void SignalTracePlot::setMarkPeakDb(float markPeakDb)
{
    m_markPeakDb = markPeakDb;
    update();
}

void SignalTracePlot::setShowThreshold(bool show)
{
    m_showThreshold = show;
    update();
}

void SignalTracePlot::setMaskOverlay(bool enabled, double ditSec, double dahSec,
                                     float noiseFloorDb, float peakDb,
                                     float ditScore, float dahScore,
                                     bool looking, bool ditHit, bool dahHit)
{
    m_maskEnabled = enabled;
    m_maskDitSec = std::max(0.015, ditSec);
    m_maskDahSec = std::max(m_maskDitSec + 0.010, dahSec);
    m_maskFloorDb = noiseFloorDb;
    m_maskPeakDb = peakDb;
    m_maskDitScore = ditScore;
    m_maskDahScore = dahScore;
    m_maskLooking = looking;
    m_maskDitHit = ditHit;
    m_maskDahHit = dahHit;
    update();
}

float SignalTracePlot::lastPower() const
{
    const QVector<float> &s = displaySamples();
    return s.isEmpty() ? -200.0f : s.last();
}

float SignalTracePlot::sampleAt(int index) const
{
    const QVector<float> &s = displaySamples();
    if (index < 0 || index >= s.size()) {
        return -200.0f;
    }
    return s[index];
}

double SignalTracePlot::samplePeriodSec() const
{
    /*
     * Fixed time base: spanSeconds maps to m_maxSamples slots.
     * Typical: 10 s × 50 Hz → 0.02 s/sample. Independent of how many
     * samples have been received so far (no stretch-to-fill).
     */
    if (m_maxSamples <= 1 || m_spanSeconds <= 0) {
        return 0.02;
    }
    return static_cast<double>(m_spanSeconds) / static_cast<double>(m_maxSamples);
}

int SignalTracePlot::midSampleIndex() const
{
    const int n = displaySamples().size();
    if (n <= 0 || m_spanSeconds <= 0) {
        return -1;
    }
    /*
     * DETECT is at fixed mid-screen = age span/2 from "now" (right edge).
     * Do not use n/2 — that moves as the buffer fills and breaks mask alignment.
     */
    const double dt = samplePeriodSec();
    if (dt <= 1e-9) {
        return -1;
    }
    const int samplesBack = static_cast<int>(std::lround((0.5 * m_spanSeconds) / dt));
    const int idx = n - 1 - samplesBack;
    if (idx < 0 || idx >= n) {
        return -1; /* trace has not yet reached the mid cursor */
    }
    return idx;
}

qreal SignalTracePlot::sampleToX(int index, int nSamples, const QRect &plot) const
{
    if (nSamples <= 0 || m_spanSeconds <= 0 || plot.width() <= 0) {
        return plot.right();
    }
    const double dt = samplePeriodSec();
    const double ageSec = static_cast<double>(nSamples - 1 - index) * dt;
    const double pxPerSec = static_cast<double>(plot.width())
                            / static_cast<double>(m_spanSeconds);
    return static_cast<qreal>(plot.right()) - ageSec * pxPerSec;
}

float SignalTracePlot::peakPower() const
{
    const QVector<float> &s = displaySamples();
    if (s.isEmpty()) {
        return -200.0f;
    }
    float p = s.first();
    for (float v : s) {
        p = std::max(p, v);
    }
    return p;
}

float SignalTracePlot::minPower() const
{
    const QVector<float> &s = displaySamples();
    if (s.isEmpty()) {
        return -200.0f;
    }
    float p = s.first();
    for (float v : s) {
        p = std::min(p, v);
    }
    return p;
}

void SignalTracePlot::trimToMax()
{
    if (m_frozen) {
        return;
    }
    while (m_samples.size() > m_maxSamples) {
        m_samples.removeFirst();
    }
}

QRect SignalTracePlot::plotRect() const
{
    return QRect(56, 12, std::max(1, width() - 72), std::max(1, height() - 48));
}

void SignalTracePlot::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.fillRect(rect(), m_frozen ? QColor(20, 18, 8) : QColor(12, 12, 14));

    const QRect plot = plotRect();
    painter.fillRect(plot, QColor(0, 0, 0));

    const QVector<float> &samples = displaySamples();
    const float yMin = m_frozen ? m_frozenMinDb : m_displayMinDb;
    const float yMax = m_frozen ? m_frozenMaxDb : m_displayMaxDb;
    const float ySpan = std::max(1.0f, yMax - yMin);

    auto dbToY = [&](float db) -> int {
        const float t = (db - yMin) / ySpan;
        const int y = plot.bottom() - static_cast<int>(t * plot.height());
        return std::max(plot.top(), std::min(plot.bottom(), y));
    };

    painter.setFont(QFont("Courier", 8));
    for (int i = 0; i <= 4; ++i) {
        const float db = yMin + (ySpan * i) / 4.0f;
        const int y = dbToY(db);
        painter.setPen(QPen(QColor(45, 45, 50), 1, Qt::DotLine));
        painter.drawLine(plot.left(), y, plot.right(), y);
        painter.setPen(QColor(180, 180, 180));
        painter.drawText(4, y + 4, QString::number(db, 'f', 0));
    }

    /* Time grid for hand reading (seconds across span) */
    const int spanSec = std::max(1, m_spanSeconds);
    for (int i = 0; i <= 4; ++i) {
        const int x = plot.left() + (i * plot.width()) / 4;
        painter.setPen(QPen(QColor(45, 45, 50), 1, Qt::DotLine));
        painter.drawLine(x, plot.top(), x, plot.bottom());
        const float ageSec = spanSec * (1.0f - static_cast<float>(i) / 4.0f);
        painter.setPen(QColor(140, 140, 150));
        painter.drawText(x - 10, plot.bottom() + 14, QString::number(ageSec, 'f', 0) + "s");
    }

    if (m_noiseFloorDb > yMin && m_noiseFloorDb < yMax) {
        const int ny = dbToY(m_noiseFloorDb);
        painter.setPen(QPen(QColor(200, 140, 40), 1, Qt::DashLine));
        painter.drawLine(plot.left(), ny, plot.right(), ny);
        painter.setPen(QColor(220, 170, 60));
        painter.drawText(plot.right() - 72, ny - 4, "noise");
    }

    /* Top of dits/dahs — absolute mark peak */
    if (m_markPeakDb > -180.0f && m_markPeakDb > yMin && m_markPeakDb < yMax + 5.0f) {
        const int py = dbToY(m_markPeakDb);
        painter.setPen(QPen(QColor(80, 200, 255), 2, Qt::DashLine));
        painter.drawLine(plot.left(), py, plot.right(), py);
        painter.setPen(QColor(120, 220, 255));
        painter.setFont(QFont("Courier", 8, QFont::Bold));
        painter.drawText(plot.left() + 4, std::max(plot.top() + 12, py - 4),
                         QString("PEAK %1 dB").arg(m_markPeakDb, 0, 'f', 1));
    }

    /* Mid-scope detection cursor (threshold + mask evaluate here) */
    const int midX = plot.left() + plot.width() / 2;
    painter.setPen(QPen(QColor(255, 220, 80, 180), 1, Qt::DashLine));
    painter.drawLine(midX, plot.top(), midX, plot.bottom());
    painter.setPen(QColor(255, 220, 80));
    painter.setFont(QFont("Courier", 8, QFont::Bold));
    painter.drawText(midX + 4, plot.top() + 12, "DETECT");

    /* Auto/manual threshold — must cut through mark peaks (below PEAK) */
    if (m_showThreshold) {
        const int ty = dbToY(m_thresholdDb);
        painter.setPen(QPen(QColor(255, 80, 80), 2, Qt::SolidLine));
        painter.drawLine(plot.left(), ty, plot.right(), ty);
        painter.setPen(QColor(255, 120, 120));
        painter.setFont(QFont("Courier", 8, QFont::Bold));
        painter.drawText(plot.left() + 4, ty - 4,
                         QString("THRESH %1 dB").arg(m_thresholdDb, 0, 'f', 1));
        painter.setPen(QColor(0, 200, 100));
        painter.drawText(midX + 6, std::max(plot.top() + 12, ty - 8), "HIGH");
        painter.setPen(QColor(120, 120, 200));
        painter.drawText(midX + 6, std::min(plot.bottom() - 4, ty + 14), "LOW");
    }

    /*
     * Fixed time scale: right edge = now, left = now − spanSeconds.
     * Samples keep constant px/sec as the buffer fills (grow left from "now");
     * never stretch the short buffer across the full width (that broke mask match).
     */
    if (samples.size() >= 1) {
        const int n = samples.size();
        QPolygonF poly;
        poly.reserve(n);
        for (int i = 0; i < n; ++i) {
            const qreal x = sampleToX(i, n, plot);
            if (x < plot.left() - 2.0) {
                continue; /* older than span */
            }
            if (x > plot.right() + 2.0) {
                continue;
            }
            poly << QPointF(x, dbToY(samples[i]));
        }

        if (poly.size() >= 2) {
            QPen tracePen(m_frozen ? QColor(255, 220, 60) : QColor(0, 255, 70));
            tracePen.setWidthF(2.0);
            tracePen.setCosmetic(true);
            tracePen.setJoinStyle(Qt::RoundJoin);
            tracePen.setCapStyle(Qt::RoundCap);
            painter.setPen(tracePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(poly);
        } else if (poly.size() == 1) {
            painter.setBrush(QColor(0, 255, 70));
            painter.setPen(QPen(QColor(40, 40, 40), 1));
            painter.drawEllipse(poly.first(), 4, 4);
        }

        /* "Now" marker at right edge */
        painter.setBrush(QColor(0, 255, 70));
        painter.setPen(QPen(QColor(40, 40, 40), 1));
        painter.drawEllipse(QPoint(plot.right(), dbToY(samples.last())), 4, 4);

        /* Keying cursor only when mid-age sample exists under fixed scale */
        const int midIdx = midSampleIndex();
        if (midIdx >= 0 && midIdx < n) {
            const int cy = dbToY(samples[midIdx]);
            const bool above = samples[midIdx] >= m_thresholdDb;
            painter.setBrush(above ? QColor(255, 220, 0) : QColor(100, 140, 255));
            painter.setPen(QPen(QColor(40, 40, 40), 1));
            painter.drawEllipse(QPoint(midX, cy), 6, 6);
        }
    } else {
        painter.setPen(QColor(160, 160, 160));
        painter.setFont(QFont("Courier", 10));
        painter.drawText(plot.adjusted(12, 12, -12, -12),
                         Qt::AlignCenter,
                         "Click a signal on the waterfall\nto begin tracing.");
    }

    /*
     * Geometric dit/dah mask station at mid-scope DETECT cursor.
     * Shared LEFT edge at midX (onset station); boxes extend right toward now.
     * Left-align avoids false dit hits on the trailing edge of a dah.
     * Vertical: noise floor → current waveform peak (shrinks when weaker).
     */
    if (m_maskEnabled && m_spanSeconds > 0) {
        const double pxPerSec = static_cast<double>(plot.width())
                                / static_cast<double>(m_spanSeconds);
        /* Independent measured/manual widths — not forced 1:3 from a single unit */
        const int ditW = std::max(6, static_cast<int>(std::lround(m_maskDitSec * pxPerSec)));
        const int dahW = std::max(ditW + 4,
                                  static_cast<int>(std::lround(m_maskDahSec * pxPerSec)));
        const int originX = midX; /* shared LEFT edge of dit & dah */

        /* Bottom = noise floor (not display min / thr). */
        float floorDb = m_maskFloorDb;
        if (floorDb < -180.0f || !std::isfinite(floorDb)) {
            floorDb = m_noiseFloorDb;
        }
        if (m_noiseFloorDb > -180.0f) {
            /* Prefer live scope noise when available */
            floorDb = 0.5f * floorDb + 0.5f * m_noiseFloorDb;
        }

        /* Top = current peak near the mask / detection region (varies with strength). */
        float peakDb = m_maskPeakDb;
        if (!samples.isEmpty()) {
            const int n = samples.size();
            const int midIdx = midSampleIndex();
            if (midIdx >= 0) {
                /* Peak of envelope from mid toward "now" (body of mark under masks) */
                const int endIdx = std::min(n - 1, midIdx + std::max(8, n / 5));
                for (int i = midIdx; i <= endIdx; ++i) {
                    peakDb = std::max(peakDb, samples[i]);
                }
                const int back = std::max(0, midIdx - n / 10);
                for (int i = back; i < midIdx; ++i) {
                    peakDb = std::max(peakDb, samples[i]);
                }
            } else {
                /* Trace not to mid yet — use recent right-edge peak */
                const int take = std::min(n, 40);
                for (int i = n - take; i < n; ++i) {
                    peakDb = std::max(peakDb, samples[i]);
                }
            }
        }
        if (!std::isfinite(peakDb) || peakDb < floorDb + 1.5f) {
            peakDb = floorDb + 6.0f;
        }
        /* Clamp into drawable range but do not pin to display max */
        if (peakDb > yMax) {
            peakDb = yMax;
        }
        if (floorDb < yMin) {
            floorDb = yMin;
        }

        const int bot = std::min(plot.bottom(), std::max(plot.top(), dbToY(floorDb)));
        const int top = std::min(bot - 6, std::max(plot.top(), dbToY(peakDb)));
        const int h = std::max(6, bot - top);

        const bool ditGreen = m_maskDitHit;
        const bool dahGreen = m_maskDahHit;
        const QColor ditFill = ditGreen ? QColor(40, 220, 80, 70) : QColor(230, 40, 40, 55);
        const QColor dahFill = dahGreen ? QColor(40, 220, 80, 45) : QColor(230, 40, 40, 35);
        const QColor ditEdge = ditGreen ? QColor(80, 255, 120) : QColor(255, 60, 60);
        const QColor dahEdge = dahGreen ? QColor(80, 255, 120) : QColor(255, 90, 90);

        /* Clip mask width to plot right edge */
        const int maxW = std::max(8, plot.right() - originX);
        const int dahDrawW = std::min(dahW, maxW);
        const int ditDrawW = std::min(ditW, maxW);

        /* Dah (wider) under dit — shared LEFT edge at originX, extend right */
        QRect dahRect(originX, top, dahDrawW, h);
        painter.setBrush(dahFill);
        painter.setPen(QPen(dahEdge, dahGreen ? 3 : 2));
        painter.drawRect(dahRect);
        {
            QPolygonF trap;
            const int rise = std::max(3, dahDrawW / 6);
            trap << QPointF(originX, bot)
                 << QPointF(originX + rise, top)
                 << QPointF(originX + dahDrawW - rise, top)
                 << QPointF(originX + dahDrawW, bot);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(dahEdge, 1, Qt::DashLine));
            painter.drawPolygon(trap);
        }

        QRect ditRect(originX, top, ditDrawW, h);
        painter.setBrush(ditFill);
        painter.setPen(QPen(ditEdge, ditGreen ? 3 : 2));
        painter.drawRect(ditRect);
        {
            QPolygonF trap;
            const int rise = std::max(2, ditDrawW / 5);
            trap << QPointF(originX, bot)
                 << QPointF(originX + rise, top)
                 << QPointF(originX + ditDrawW - rise, top)
                 << QPointF(originX + ditDrawW, bot);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(ditEdge, 1));
            painter.drawPolygon(trap);
        }

        painter.setFont(QFont("Courier", 8, QFont::Bold));
        painter.setPen(ditEdge);
        painter.drawText(originX + 2, std::max(plot.top() + 12, top - 3),
                         QString("DIT %1ms s=%2")
                             .arg(m_maskDitSec * 1000.0, 0, 'f', 0)
                             .arg(m_maskDitScore, 0, 'f', 2));
        painter.setPen(dahEdge);
        painter.drawText(originX + ditDrawW + 4, std::min(plot.bottom() - 4, bot + 12),
                         QString("DAH %1ms s=%2 %3")
                             .arg(m_maskDahSec * 1000.0, 0, 'f', 0)
                             .arg(m_maskDahScore, 0, 'f', 2)
                             .arg(ditGreen ? QStringLiteral("[HIT DIT]")
                                  : dahGreen ? QStringLiteral("[HIT DAH]")
                                  : m_maskLooking ? QStringLiteral("[LOOKING]")
                                                  : QStringLiteral("")));
    }

    painter.setPen(QPen(m_frozen ? QColor(255, 200, 0) : QColor(200, 200, 200), m_frozen ? 2 : 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot.adjusted(0, 0, -1, -1));
    painter.setFont(QFont("Courier", 8));
    painter.setPen(QColor(180, 180, 180));
    painter.drawText(plot.left(), plot.bottom() + 26, "older");
    painter.drawText(midX - 18, plot.bottom() + 26, "mid");
    painter.drawText(plot.right() - 40, plot.bottom() + 26, "now");
    painter.drawText(8, 12, "dB");

    if (m_frozen) {
        painter.fillRect(QRect(plot.left() + 4, plot.top() + 4, 200, 22), QColor(40, 30, 0, 200));
        painter.setPen(QColor(255, 210, 0));
        painter.setFont(QFont("Courier", 12, QFont::Bold));
        painter.drawText(plot.left() + 10, plot.top() + 20,
                         QString("FROZEN — %1 samples").arg(samples.size()));
    }
}

/* ------------------------------------------------------------------ */
/* SignalTraceWindow                                                   */
/* ------------------------------------------------------------------ */

SignalTraceWindow::SignalTraceWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_plot(nullptr)
    , m_freqLabel(nullptr)
    , m_statsLabel(nullptr)
    , m_freqSliderLabel(nullptr)
    , m_binsSliderLabel(nullptr)
    , m_threshSliderLabel(nullptr)
    , m_freezeButton(nullptr)
    , m_clearButton(nullptr)
    , m_morseButton(nullptr)

    , m_playButton(nullptr)
    , m_stopPlayButton(nullptr)
    , m_afcCheck(nullptr)
    , m_autoThreshCheck(nullptr)
    , m_spanCombo(nullptr)
    , m_freqSlider(nullptr)
    , m_binsSlider(nullptr)
    , m_thresholdSlider(nullptr)
    , m_afcSearchSlider(nullptr)
    , m_afcLabel(nullptr)
    , m_hasTarget(false)
    , m_frozen(false)
    , m_recording(false)
    , m_afcEnabled(true)
    , m_afcLocked(false)
    , m_keyIsHigh(false)
    , m_baseOffsetHz(0.0f)
    , m_baseAbsHz(0.0f)
    , m_offsetHz(0.0f)
    , m_absFreqHz(0.0f)
    , m_binWidthHz(46.875f)
    , m_afcLockHz(0.0f)
    , m_afcPeakPowerDb(-200.0f)
    , m_afcSnrDb(0.0f)
    , m_lastPowerDb(-200.0f)
    , m_peakPowerDb(-200.0f)
    , m_minPowerDb(0.0f)
    , m_noiseFloorDb(-95.0f)
    , m_thresholdDb(-70.0f)  /* near noise+14 valley; avoid Schmitt gap-merge */
    , m_trackLowDb(-95.0f)
    , m_trackHighDb(-70.0f)
    , m_noiseEmaDb(-95.0f)
    , m_noiseAvgDb(-200.0f)
    , m_noisePeakDb(-200.0f)
    , m_markPeakHoldDb(-200.0f)
    , m_markBodyDb(-200.0f)
    , m_trackInit(false)
    , m_sampleCount(0)
    , m_afcHoldCount(0)
    , m_keyPendingHigh(false)
    , m_keyEdgeStartMs(-1)
    , m_lastDetectTimeMs(-1)
    , m_refWpm(20)
    , m_playbackTimer(nullptr)
    , m_playbackActive(false)
    , m_playbackInjecting(false)
    , m_playbackIndex(0)
    , m_playbackWallStartMs(0)
    , m_playbackNoiseFloorDb(-80.0f)
    , m_playbackSpeed(1.0f)
{
    setWindowTitle("CW Signal Trace");
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowModality(Qt::NonModal);
    resize(760, 540);

    m_plot = new SignalTracePlot(this);

    m_freqLabel = new QLabel("No signal selected — click the waterfall", this);
    m_freqLabel->setStyleSheet(
        "color: #00008b; font-family: 'JetBrains Mono', Tahoma, monospace; "
        "font-size: 12px; font-weight: bold;");

    m_statsLabel = new QLabel("", this);
    m_statsLabel->setStyleSheet(
        "color: #00008b; font-family: 'JetBrains Mono', Tahoma, monospace; "
        "font-size: 11px;");

    m_freezeButton = new QPushButton("Freeze", this);
    m_freezeButton->setCheckable(true);
    m_clearButton = new QPushButton("Clear", this);
    /* Morse decoder + Record live on the main window (spectrum bar / toolbar) */
    m_morseButton = nullptr;
    m_playButton = new QPushButton("Play capture…", this);
    m_playButton->setToolTip(
        "Play a saved .wav / .cwtrace through this scope as a live signal.\n"
        "Feeds the same path as RF: plot → threshold → Morse decoder.\n"
        "Prefers the sibling .cwtrace (real power envelope) when present.");
    m_stopPlayButton = new QPushButton("Stop play", this);
    m_stopPlayButton->setEnabled(false);
    m_stopPlayButton->setToolTip("Stop offline capture playback");

    m_spanCombo = new QComboBox(this);
    m_spanCombo->addItem("5 s", 5);
    m_spanCombo->addItem("10 s", 10);
    m_spanCombo->addItem("20 s", 20);
    m_spanCombo->addItem("40 s", 40);
    m_spanCombo->setCurrentIndex(1);

    /* Frequency fine-tune: ±40 steps × 50 Hz = ±2000 Hz */
    m_freqSlider = new QSlider(Qt::Horizontal, this);
    m_freqSlider->setRange(-40, 40);
    m_freqSlider->setValue(0);
    m_freqSlider->setTickInterval(10);
    m_freqSlider->setTickPosition(QSlider::TicksBelow);
    m_freqSlider->setPageStep(1);
    m_freqSlider->setToolTip("Shift listen frequency in 50 Hz steps");
    m_freqSliderLabel = new QLabel("Freq fine: 0 Hz", this);
    m_freqSliderLabel->setMinimumWidth(140);

    /* Bandwidth in half-bins */
    m_binsSlider = new QSlider(Qt::Horizontal, this);
    m_binsSlider->setRange(0, 16);
    m_binsSlider->setValue(1);
    m_binsSlider->setTickInterval(2);
    m_binsSlider->setTickPosition(QSlider::TicksBelow);
    m_binsSlider->setToolTip("Number of FFT bins (±) to include around the tone");
    m_binsSliderLabel = new QLabel("Bins ±1", this);
    m_binsSliderLabel->setMinimumWidth(140);

    /* Threshold in 0.1 dB units; range expanded live (deskHPSDR audio can be >0 dB) */
    m_thresholdSlider = new QSlider(Qt::Horizontal, this);
    m_thresholdSlider->setRange(-1200, 400);  /* -120.0 … +40.0 dB */
    m_thresholdSlider->setValue(-800);
    m_thresholdSlider->setToolTip(
        "High/low decision threshold (red line).\n"
        "With Auto thresh ON, this tracks noise floor + mark level continuously.\n"
        "Range follows signal (can go above 0 dB on hot audio envelopes).");
    m_threshSliderLabel = new QLabel("Thresh: -80.0 dB", this);
    m_threshSliderLabel->setMinimumWidth(140);

    m_autoThreshCheck = new QCheckBox("Auto thresh", this);
    m_autoThreshCheck->setChecked(true);
    m_autoThreshCheck->setToolTip(
        "Continuously place the threshold between the tracked noise floor\n"
        "and mark peaks, with hysteresis. Uncheck to set threshold by hand.");

    /* AFC: peak-lock strongest tone near click (handles ±few hundred Hz mark error) */
    m_afcCheck = new QCheckBox("AFC peak-lock", this);
    m_afcCheck->setChecked(true);
    m_afcCheck->setToolTip(
        "Track the strongest signal within the search window around the click.\n"
        "Slews the listen frequency to peak the CW envelope (holds during key-up).");
    m_afcSearchSlider = new QSlider(Qt::Horizontal, this);
    m_afcSearchSlider->setRange(2, 16);   /* ×50 Hz → ±100 … ±800 Hz */
    m_afcSearchSlider->setValue(8);       /* ±400 Hz default */
    m_afcSearchSlider->setTickInterval(2);
    m_afcSearchSlider->setTickPosition(QSlider::TicksBelow);
    m_afcSearchSlider->setToolTip("AFC search window ±Hz around current lock");
    m_afcLabel = new QLabel("AFC ±400 Hz", this);
    m_afcLabel->setMinimumWidth(140);

    auto *sliderGrid = new QGridLayout();
    sliderGrid->addWidget(new QLabel("Freq ±50 Hz:", this), 0, 0);
    sliderGrid->addWidget(m_freqSlider, 0, 1);
    sliderGrid->addWidget(m_freqSliderLabel, 0, 2);
    sliderGrid->addWidget(new QLabel("Listen bins ±:", this), 1, 0);
    sliderGrid->addWidget(m_binsSlider, 1, 1);
    sliderGrid->addWidget(m_binsSliderLabel, 1, 2);
    sliderGrid->addWidget(m_autoThreshCheck, 2, 0);
    sliderGrid->addWidget(m_thresholdSlider, 2, 1);
    sliderGrid->addWidget(m_threshSliderLabel, 2, 2);
    sliderGrid->addWidget(m_afcCheck, 3, 0);
    sliderGrid->addWidget(m_afcSearchSlider, 3, 1);
    sliderGrid->addWidget(m_afcLabel, 3, 2);
    sliderGrid->setColumnStretch(1, 1);

    auto *controls = new QHBoxLayout();
    controls->addWidget(new QLabel("Span:", this));
    controls->addWidget(m_spanCombo);
    controls->addStretch(1);
    controls->addWidget(m_playButton);
    controls->addWidget(m_stopPlayButton);
    controls->addWidget(m_freezeButton);
    controls->addWidget(m_clearButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_freqLabel);
    /* Status / SNR / thr / state line removed — RF frequency only above the plot */
    if (m_statsLabel) {
        m_statsLabel->hide();
    }
    layout->addWidget(m_plot, 1);
    layout->addLayout(sliderGrid);
    layout->addLayout(controls);
    setLayout(layout);

    connect(m_freezeButton, &QPushButton::toggled, this, &SignalTraceWindow::onFreezeToggled);
    connect(m_clearButton, &QPushButton::clicked, this, &SignalTraceWindow::onClearClicked);
    connect(m_spanCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SignalTraceWindow::onSpanChanged);
    connect(m_freqSlider, &QSlider::valueChanged, this, &SignalTraceWindow::onFreqSliderChanged);
    connect(m_binsSlider, &QSlider::valueChanged, this, &SignalTraceWindow::onBinsSliderChanged);
    connect(m_thresholdSlider, &QSlider::valueChanged, this, &SignalTraceWindow::onThresholdSliderChanged);
    connect(m_playButton, &QPushButton::clicked, this, &SignalTraceWindow::onPlayCaptureClicked);
    connect(m_stopPlayButton, &QPushButton::clicked, this, &SignalTraceWindow::onStopPlaybackClicked);
    connect(m_afcCheck, &QCheckBox::toggled, this, &SignalTraceWindow::onAfcToggled);
    connect(m_afcSearchSlider, &QSlider::valueChanged, this, &SignalTraceWindow::onAfcSearchChanged);
    connect(m_autoThreshCheck, &QCheckBox::toggled, this, &SignalTraceWindow::onAutoThreshToggled);

    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setInterval(10);  /* 100 Hz poll; feeds samples by capture timestamps */
    connect(m_playbackTimer, &QTimer::timeout, this, &SignalTraceWindow::onPlaybackTick);

    m_plot->setMaxSamples(maxSamplesForSpan());
    m_plot->setSpanSeconds(m_spanCombo ? m_spanCombo->currentData().toInt() : 10);
    m_plot->setThresholdDb(m_thresholdDb);
    m_freezeButton->setToolTip(
        "Freeze: lock the scope for hand-copy.\n"
        "Resume: continue live scrolling.\n"
        "While frozen, live samples and retune are blocked.");
    m_clock.start();
    updateTitleAndLabels();
}

float SignalTraceWindow::targetOffsetHz() const
{
    return m_offsetHz;
}

float SignalTraceWindow::targetAbsFreqHz() const
{
    return m_absFreqHz;
}

int SignalTraceWindow::halfBins() const
{
    return m_binsSlider ? m_binsSlider->value() : 1;
}

bool SignalTraceWindow::afcEnabled() const
{
    return m_afcEnabled && m_afcCheck && m_afcCheck->isChecked();
}

float SignalTraceWindow::afcSearchHalfHz() const
{
    const int steps = m_afcSearchSlider ? m_afcSearchSlider->value() : 8;
    return static_cast<float>(steps) * 50.0f;
}

int SignalTraceWindow::maxSamplesForSpan() const
{
    const int seconds = m_spanCombo ? m_spanCombo->currentData().toInt() : 10;
    return std::max(100, seconds * 50);
}

void SignalTraceWindow::setMaskOverlay(bool enabled, double ditSec, double dahSec,
                                       float noiseFloorDb, float peakDb,
                                       float ditScore, float dahScore,
                                       bool looking, bool ditHit, bool dahHit)
{
    if (m_plot) {
        m_plot->setMaskOverlay(enabled, ditSec, dahSec, noiseFloorDb, peakDb,
                               ditScore, dahScore, looking, ditHit, dahHit);
    }
}

void SignalTraceWindow::setTarget(float freqOffsetHz, float absFreqHz, float binWidthHz)
{
    /* New waterfall click always starts live */
    if (m_freezeButton) {
        m_freezeButton->blockSignals(true);
        m_freezeButton->setChecked(false);
        m_freezeButton->blockSignals(false);
    }
    m_frozen = false;
    if (m_plot) {
        m_plot->setFrozen(false);
    }
    applyFrozenUi(false);

    m_hasTarget = true;
    m_baseOffsetHz = freqOffsetHz;
    m_baseAbsHz = absFreqHz;
    m_offsetHz = freqOffsetHz;
    m_absFreqHz = absFreqHz;
    m_afcLockHz = freqOffsetHz;
    m_afcLocked = false;
    m_afcHoldCount = 0;
    m_afcPeakPowerDb = -200.0f;
    m_afcSnrDb = 0.0f;
    if (binWidthHz > 0.0f) {
        m_binWidthHz = binWidthHz;
    }
    m_lastPowerDb = -200.0f;
    m_peakPowerDb = -200.0f;
    m_minPowerDb = 0.0f;
    m_sampleCount = 0;
    m_trackInit = false;
    m_keyIsHigh = false;
    m_keyPendingHigh = false;
    m_keyEdgeStartMs = -1;
    m_lastDetectTimeMs = -1;
    m_trackLowDb = -95.0f;
    m_trackHighDb = -70.0f;
    m_noiseEmaDb = -95.0f;
    m_noiseAvgDb = -200.0f;
    m_noisePeakDb = -200.0f;
    m_markPeakHoldDb = -200.0f;
    m_markBodyDb = -200.0f;
    m_powerHist.clear();
    m_sampleTimes.clear();
    if (m_plot) {
        m_plot->setMarkPeakDb(-200.0f);
    }

    if (m_freqSlider) {
        m_freqSlider->blockSignals(true);
        m_freqSlider->setValue(0);
        m_freqSlider->blockSignals(false);
    }
    if (m_freqSliderLabel) {
        m_freqSliderLabel->setText("Freq fine: 0 Hz");
    }

    if (m_plot) {
        m_plot->clear();
        m_plot->setMaxSamples(maxSamplesForSpan());
        if (m_spanCombo) {
            m_plot->setSpanSeconds(m_spanCombo->currentData().toInt());
        }
    }
    m_capture.clear();
    m_clock.restart();
    updateTitleAndLabels();
}

void SignalTraceWindow::applyAfcStep(float peakOffsetHz, float peakPowerDb, float snrDb)
{
    m_afcPeakPowerDb = peakPowerDb;
    m_afcSnrDb = snrDb;

    /*
     * PLL-style loop filter:
     *  - Only slew when peak has usable SNR (key-down / carrier present)
     *  - Hold last lock during key-up so we don't walk toward noise
     *  - Soft gain so lock doesn't chatter between bins
     */
    const float minSnrDb = 5.0f;
    const float loopGain = 0.22f;   /* fraction of error applied per spectrum column */
    const float maxSlewHz = 80.0f;  /* cap step size */

    if (snrDb >= minSnrDb) {
        float err = peakOffsetHz - m_afcLockHz;
        float step = loopGain * err;
        if (step > maxSlewHz) {
            step = maxSlewHz;
        } else if (step < -maxSlewHz) {
            step = -maxSlewHz;
        }
        m_afcLockHz += step;
        m_afcLocked = true;
        m_afcHoldCount = 0;
    } else {
        /* Hold — do not update lock frequency */
        m_afcHoldCount++;
    }

    /* Keep lock within search of original click (don't wander to adjacent stations) */
    const float search = afcSearchHalfHz();
    const float lo = m_baseOffsetHz - search;
    const float hi = m_baseOffsetHz + search;
    m_afcLockHz = std::max(lo, std::min(hi, m_afcLockHz));

    m_offsetHz = m_afcLockHz;
    m_absFreqHz = m_baseAbsHz + (m_afcLockHz - m_baseOffsetHz);
}

float SignalTraceWindow::processSpectrumColumn(const QVector<float> &spectrum, float binWidth,
                                               float noiseFloorDb)
{
    if (!m_hasTarget || m_frozen || spectrum.isEmpty()) {
        return m_offsetHz;
    }
    if (binWidth > 0.0f) {
        m_binWidthHz = binWidth;
    }

    float powerDb = -200.0f;
    const int hb = halfBins();

    if (afcEnabled()) {
        float peakPower = -200.0f;
        float snr = 0.0f;
        const float peak = SpectrumWidget::peakOffsetNear(
            spectrum, m_afcLockHz, m_binWidthHz, afcSearchHalfHz(),
            &peakPower, noiseFloorDb, &snr);
        applyAfcStep(peak, peakPower, snr);
        powerDb = SpectrumWidget::powerAtOffset(spectrum, m_offsetHz, m_binWidthHz, hb);
        emit afcOffsetChanged(m_offsetHz);
    } else {
        powerDb = SpectrumWidget::powerAtOffset(spectrum, m_offsetHz, m_binWidthHz, hb);
    }

    appendSample(powerDb, noiseFloorDb);
    return m_offsetHz;
}

void SignalTraceWindow::appendSample(float powerDb, float noiseFloorDb, qint64 forceTimeMs)
{
    /*
     * Hard stop while frozen or during offline playback for live RF samples.
     * Playback injects via injectPlaybackSample (m_playbackInjecting=true).
     */
    if (!m_hasTarget || !m_plot) {
        return;
    }
    if (!m_playbackInjecting && (m_frozen || m_playbackActive)) {
        return;
    }
    if (!std::isfinite(powerDb)) {
        return;
    }

    if (noiseFloorDb > -180.0f) {
        m_noiseFloorDb = noiseFloorDb;
        m_plot->setNoiseFloor(noiseFloorDb);
    }

    /* Playback must use capture-relative times so mark/space durations stay true */
    const qint64 tMs = (forceTimeMs >= 0) ? forceTimeMs : m_clock.elapsed();

    m_plot->append(powerDb);
    m_sampleTimes.append(tMs);
    const int maxN = maxSamplesForSpan();
    while (m_sampleTimes.size() > maxN) {
        m_sampleTimes.removeFirst();
    }
    /* Keep timestamps aligned with plot sample count */
    while (m_sampleTimes.size() > m_plot->sampleCount()) {
        m_sampleTimes.removeFirst();
    }

    m_lastPowerDb = powerDb;
    if (m_sampleCount == 0) {
        m_peakPowerDb = powerDb;
        m_minPowerDb = powerDb;
    } else {
        m_peakPowerDb = std::max(m_peakPowerDb, powerDb);
        m_minPowerDb = std::min(m_minPowerDb, powerDb);
    }
    m_sampleCount++;

    /*
     * Auto thr always adapts when checked (live RF and capture playback).
     * Manual thr freezes at the slider / .cwtrace header value.
     * Dual-timescale mark body freezes on key-up so playback no longer races
     * thr into the noise floor between letters.
     */
    if (m_autoThreshCheck && m_autoThreshCheck->isChecked()) {
        updateAdaptiveThreshold(powerDb, noiseFloorDb);
    }

    /*
     * Mid-scope detection under FIXED time scale: sample at age = span/2
     * (DETECT line). Until the trace has grown left past mid, do not key
     * or feed Morse — using "now" would desync masks from the waveform.
     */
    const int midIdx = m_plot->midSampleIndex();
    float detectPower = powerDb;
    qint64 detectTimeMs = tMs;
    bool haveMid = false;
    if (midIdx >= 0) {
        detectPower = m_plot->sampleAt(midIdx);
        if (midIdx < m_sampleTimes.size()) {
            detectTimeMs = m_sampleTimes[midIdx];
        }
        haveMid = true;
    }

    /*
     * Only advance keying / decoder when the mid-scope sample is new
     * (avoids re-processing the same mid sample).
     */
    const bool newDetectSample = haveMid && (detectTimeMs != m_lastDetectTimeMs);
    if (newDetectSample) {
        m_lastDetectTimeMs = detectTimeMs;

        /*
         * Schmitt + min-dit debounce (ref WPM) on mid-scope sample:
         *   go HIGH only above thresh + hyst
         *   go LOW only below thresh - hyst
         * Then require the new state to hold ≥ ~0.45×dit (ref WPM).
         */
        /* 1.5 dB: 2 dB ate short dits when thr sat near mark tops (T/E spam). */
        const float hystDb = 1.5f;
        bool wantHigh;
        if (m_keyIsHigh) {
            wantHigh = detectPower >= (m_thresholdDb - hystDb);
        } else {
            wantHigh = detectPower >= (m_thresholdDb + hystDb);
        }

        const double ditMs = 1200.0 / static_cast<double>(std::max(5, std::min(40, m_refWpm)));
        const qint64 confirmMs = static_cast<qint64>(
            std::lround(ditMs * (wantHigh ? 0.45 : 0.35)));
        const qint64 confirmClamped = std::max(qint64(18), std::min(qint64(80), confirmMs));

        if (wantHigh == m_keyIsHigh) {
            m_keyEdgeStartMs = -1;
        } else {
            if (m_keyEdgeStartMs < 0 || wantHigh != m_keyPendingHigh) {
                m_keyEdgeStartMs = detectTimeMs;
                m_keyPendingHigh = wantHigh;
            } else if (detectTimeMs - m_keyEdgeStartMs >= confirmClamped) {
                m_keyIsHigh = wantHigh;
                m_keyEdgeStartMs = -1;
            }
        }
    }
    const bool above = m_keyIsHigh;

    if (m_recording) {
        TraceSample s;
        s.timeMs = tMs;
        s.powerDb = powerDb;
        s.aboveThreshold = above;
        m_capture.append(s);
        while (m_capture.size() > kMaxCaptureSamples) {
            m_capture.removeFirst();
        }
    }

    if (newDetectSample) {
        emit sampleReady(detectPower, above, detectTimeMs);
    }
    updateTitleAndLabels();
}

void SignalTraceWindow::seedHistory(const QVector<float> &powerDbSamples, float noiseFloorDb)
{
    if (!m_hasTarget || !m_plot || m_frozen) {
        return;
    }
    if (noiseFloorDb > -180.0f) {
        m_noiseFloorDb = noiseFloorDb;
        m_plot->setNoiseFloor(noiseFloorDb);
        m_noiseEmaDb = noiseFloorDb;
    }
    m_plot->seed(powerDbSamples);
    m_sampleCount = powerDbSamples.size();
    /* Synthetic times so mid-scope detection has a lag timeline after reseed */
    m_sampleTimes.clear();
    m_sampleTimes.reserve(powerDbSamples.size());
    const qint64 baseMs = m_clock.elapsed();
    const qint64 dtMs = 20; /* ~50 Hz envelope */
    for (int i = 0; i < powerDbSamples.size(); ++i) {
        m_sampleTimes.append(baseMs - (powerDbSamples.size() - 1 - i) * dtMs);
    }
    if (!powerDbSamples.isEmpty()) {
        m_lastPowerDb = powerDbSamples.last();
        m_peakPowerDb = m_plot->peakPower();
        m_minPowerDb = m_plot->minPower();
        m_trackLowDb = m_minPowerDb;
        m_trackHighDb = m_peakPowerDb;
        m_trackInit = true;
        /* Seed threshold from history once */
        for (float p : powerDbSamples) {
            updateAdaptiveThreshold(p, noiseFloorDb);
        }
    }

    /* Do not inject seed history into capture — only live samples after tune. */
    updateTitleAndLabels();
}

void SignalTraceWindow::updateAdaptiveThreshold(float powerDb, float noiseFloorDb)
{
    if (!m_plot) {
        return;
    }

    const bool autoOn = m_autoThreshCheck && m_autoThreshCheck->isChecked();

    m_powerHist.append(powerDb);
    while (m_powerHist.size() > kThreshHistMax) {
        m_powerHist.removeFirst();
    }

    /* Spectrum NF is only a weak prior — envelope space is usually higher. */
    if (noiseFloorDb > -180.0f) {
        if (!m_trackInit) {
            m_noiseEmaDb = noiseFloorDb;
        } else {
            m_noiseEmaDb = 0.995f * m_noiseEmaDb + 0.005f * noiseFloorDb;
        }
    }

    if (!autoOn) {
        syncThresholdSliderRange();
        return;
    }

    if (m_powerHist.size() < 16) {
        float lo = m_powerHist.first();
        float hi = m_powerHist.first();
        for (float p : m_powerHist) {
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
        if (hi - lo >= 5.0f) {
            /*
             * Seed low enough that first marks clear thr. 0.55·span sat inside
             * the mark cloud (cwtrace_20260807_090453: thr~−59 vs peaks −55).
             */
            const float early = lo + 0.32f * (hi - lo);
            m_thresholdDb = m_trackInit ? (0.80f * m_thresholdDb + 0.20f * early) : early;
        }
        m_trackLowDb = lo;
        m_trackHighDb = hi;
        m_trackInit = true;
        if (m_plot) {
            m_plot->setThresholdDb(m_thresholdDb);
        }
        syncThresholdSliderRange();
        emit thresholdChanged(m_thresholdDb);
        return;
    }

    QVector<float> sorted = m_powerHist;
    std::sort(sorted.begin(), sorted.end());
    const int n = sorted.size();
    auto percentile = [&](float pct) -> float {
        const float idx = pct * 0.01f * static_cast<float>(n - 1);
        const int i0 = static_cast<int>(idx);
        const int i1 = std::min(n - 1, i0 + 1);
        const float f = idx - static_cast<float>(i0);
        return sorted[i0] * (1.0f - f) + sorted[i1] * f;
    };

    const float p10 = percentile(10.0f);
    const float p20 = percentile(20.0f);
    const float p40 = percentile(40.0f);
    const float p50 = percentile(50.0f);
    const float spaceDb = 0.5f * (p10 + p20);

    /*
     * Average noise (0% of scale) + noise peak guard.
     * Average of lower cluster = "grass" baseline for the 85/15 thr placement.
     */
    {
        float sumLo = 0.0f;
        int nLo = 0;
        float localNoisePeak = -200.0f;
        for (float p : m_powerHist) {
            if (p <= p40 + 1.0f) {
                sumLo += p;
                nLo++;
                localNoisePeak = std::max(localNoisePeak, p);
            }
        }
        if (nLo > 0) {
            const float localAvg = sumLo / static_cast<float>(nLo);
            if (m_noiseAvgDb < -180.0f) {
                m_noiseAvgDb = localAvg;
            } else {
                m_noiseAvgDb = 0.96f * m_noiseAvgDb + 0.04f * localAvg;
            }
        }
        if (localNoisePeak > -180.0f) {
            if (m_noisePeakDb < -180.0f) {
                m_noisePeakDb = localNoisePeak;
            } else if (localNoisePeak >= m_noisePeakDb) {
                m_noisePeakDb = 0.85f * m_noisePeakDb + 0.15f * localNoisePeak;
            } else {
                m_noisePeakDb -= 0.002f;
                m_noisePeakDb = std::max(m_noisePeakDb, localNoisePeak);
            }
        }
    }

    const float noiseRef = (m_noiseAvgDb > -180.0f) ? m_noiseAvgDb
                         : (m_noisePeakDb > -180.0f) ? m_noisePeakDb
                         : spaceDb;

    /*
     * Mark absolute PEAK (drawn) + body (typical dit/dah tops for thr).
     * Gate marks above noise so grass does not inflate "peak".
     */
    const float markGate = noiseRef + 5.0f;
    const bool isMark = powerDb >= markGate;

    if (isMark) {
        if (m_markPeakHoldDb < -180.0f || powerDb > m_markPeakHoldDb) {
            m_markPeakHoldDb = powerDb;
        }
        if (m_markBodyDb < -180.0f) {
            m_markBodyDb = powerDb;
        } else if (powerDb >= m_markBodyDb) {
            m_markBodyDb = 0.80f * m_markBodyDb + 0.20f * powerDb;
        } else {
            m_markBodyDb = 0.97f * m_markBodyDb + 0.03f * powerDb;
        }
    } else if (m_markPeakHoldDb > -180.0f) {
        m_markPeakHoldDb -= 0.0003f;
        if (m_markPeakHoldDb < noiseRef + 3.0f) {
            m_markPeakHoldDb = -200.0f;
        }
    }

    /*
     * Mark sample stats above the grass. PEAK line = absolute max hold.
     * Bit-top for thr uses ~p70 of mark samples so thr cuts through the
     * bulk of dits/dahs, not only rare tall spikes (164845: thr sat at −53
     * while most marks were −58…−65).
     */
    float markP25 = -200.0f;
    float markP50 = -200.0f;
    float markP70 = -200.0f;
    float markP85 = -200.0f;
    {
        QVector<float> highs;
        highs.reserve(n / 3);
        for (float p : m_powerHist) {
            if (p >= markGate) {
                highs.append(p);
            }
        }
        if (highs.size() >= 3) {
            std::sort(highs.begin(), highs.end());
            auto atPct = [&](float pct) -> float {
                const float idx = pct * 0.01f * static_cast<float>(highs.size() - 1);
                const int i0 = static_cast<int>(idx);
                const int i1 = std::min(highs.size() - 1, i0 + 1);
                const float f = idx - static_cast<float>(i0);
                return highs[i0] * (1.0f - f) + highs[i1] * f;
            };
            markP25 = atPct(25.0f);
            markP50 = atPct(50.0f);
            markP70 = atPct(70.0f);
            markP85 = atPct(85.0f);
            if (m_markPeakHoldDb < -180.0f) {
                m_markPeakHoldDb = highs.last();
            } else {
                m_markPeakHoldDb = std::max(m_markPeakHoldDb, highs.last());
            }
        } else if (!highs.isEmpty()) {
            markP25 = markP50 = markP70 = markP85 = highs.last();
            m_markPeakHoldDb = std::max(m_markPeakHoldDb, highs.last());
        }
    }

    if (isMark) {
        if (m_markPeakHoldDb < -180.0f || powerDb > m_markPeakHoldDb) {
            m_markPeakHoldDb = powerDb;
        }
    } else if (m_markPeakHoldDb > -180.0f) {
        m_markPeakHoldDb -= 0.0003f;
        if (m_markPeakHoldDb < noiseRef + 3.0f) {
            m_markPeakHoldDb = -200.0f;
        }
    }

    /* Body tracks p70 of marks — typical dit/dah crest */
    if (markP70 > -180.0f) {
        if (m_markBodyDb < -180.0f) {
            m_markBodyDb = markP70;
        } else {
            m_markBodyDb = 0.85f * m_markBodyDb + 0.15f * markP70;
        }
    } else if (m_markBodyDb < -180.0f && m_markPeakHoldDb > -180.0f) {
        m_markBodyDb = m_markPeakHoldDb;
    } else if (m_markBodyDb < -180.0f) {
        m_markBodyDb = std::max(p50 + 5.0f, spaceDb + 9.0f);
    }

    /*
     * Bit-top for thr placement = typical mark crests (body), NOT absolute max.
     * Absolute PEAK is drawn separately so thr stays low enough to intersect.
     */
    const float bitTop = m_markBodyDb;

    m_trackLowDb = noiseRef;
    m_trackHighDb = (m_markPeakHoldDb > -180.0f) ? m_markPeakHoldDb : bitTop;
    m_trackInit = true;

    /*
     * Auto thr ~28% of the way from noise to mark crests:
     *   noise (avg) ........ 0%
     *   THRESH ............. ~28% of (avgPeak − noise)
     *   PEAK (top of bits) . 100%
     * Must stay BELOW most mark samples (use mark p25), not at mark median —
     * cwtrace_20260807_090453 sat thr at ~−59 inside marks (−55…−52) → T/E spam.
     */
    float avgPeak = bitTop;
    if (m_markPeakHoldDb > -180.0f) {
        avgPeak = 0.6f * bitTop + 0.4f * m_markPeakHoldDb;
    }
    if (markP70 > -180.0f) {
        avgPeak = 0.5f * avgPeak + 0.5f * markP70;
    }

    const float span = std::max(3.0f, avgPeak - noiseRef);
    float ideal = noiseRef + 0.28f * span;

    /* Soft bounds: above grass, well below mark body / absolute peak */
    ideal = std::max(ideal, noiseRef + 3.0f);
    if (m_noisePeakDb > -180.0f) {
        ideal = std::max(ideal, m_noisePeakDb + 1.5f);
    }
    ideal = std::max(ideal, m_noiseEmaDb + 3.5f);
    if (m_markPeakHoldDb > -180.0f) {
        ideal = std::min(ideal, m_markPeakHoldDb - 5.0f);
    }
    /*
     * Cap under the lower quartile of mark energy so dits/dahs fully clear thr
     * (was markP50−1 dB → thr inside the mark cloud → only peaks keyed → T/E).
     */
    if (markP25 > -180.0f) {
        ideal = std::min(ideal, markP25 - 2.0f);
    } else if (markP50 > -180.0f) {
        ideal = std::min(ideal, markP50 - 4.0f);
    }
    if (bitTop > -180.0f) {
        ideal = std::min(ideal, bitTop - 4.0f);
    }
    ideal = std::min(ideal, avgPeak - 4.0f);

    /* Duty too high → thr in noise: nudge up, but never into mark median */
    int nAbove = 0;
    int nMarkAbove = 0;
    int nMark = 0;
    for (float p : m_powerHist) {
        if (p >= m_thresholdDb) {
            nAbove++;
        }
        if (p >= markGate) {
            nMark++;
            if (p >= m_thresholdDb) {
                nMarkAbove++;
            }
        }
    }
    const float highFrac = static_cast<float>(nAbove) / static_cast<float>(n);
    if (highFrac > 0.62f && markP25 > -180.0f) {
        ideal = std::min(markP25 - 1.5f, ideal + 1.0f);
    }
    /*
     * Marks present but thr only catches a minority of mark samples → thr too
     * high (chops elements into T/E). Pull ideal down under mark p25.
     */
    if (nMark >= 6) {
        const float markClear = static_cast<float>(nMarkAbove)
                                / static_cast<float>(nMark);
        if (markClear < 0.55f && markP25 > -180.0f) {
            ideal = std::min(ideal, markP25 - 3.0f);
        }
    }

    ideal = std::min(40.0f, std::max(-130.0f, ideal));

    const float err = ideal - m_thresholdDb;
    if (err > 2.0f) {
        m_thresholdDb = 0.72f * m_thresholdDb + 0.28f * ideal;
    } else if (err > 0.0f) {
        m_thresholdDb = 0.90f * m_thresholdDb + 0.10f * ideal;
    } else if (err < -2.0f) {
        /* Drop promptly when thr is glued into mark tops */
        m_thresholdDb = 0.55f * m_thresholdDb + 0.45f * ideal;
    } else {
        m_thresholdDb = 0.92f * m_thresholdDb + 0.08f * ideal;
    }

    m_thresholdDb = std::max(m_thresholdDb, noiseRef + 2.5f);
    if (m_markPeakHoldDb > -180.0f) {
        m_thresholdDb = std::min(m_thresholdDb, m_markPeakHoldDb - 4.0f);
    }
    if (markP25 > -180.0f) {
        m_thresholdDb = std::min(m_thresholdDb, markP25 - 1.5f);
    }
    m_thresholdDb = std::min(m_thresholdDb, avgPeak - 3.5f);
    m_thresholdDb = std::min(40.0f, std::max(-130.0f, m_thresholdDb));

    if (m_plot) {
        m_plot->setThresholdDb(m_thresholdDb);
        m_plot->setMarkPeakDb(m_markPeakHoldDb);
    }
    syncThresholdSliderRange();
    emit thresholdChanged(m_thresholdDb);
}

void SignalTraceWindow::syncThresholdSliderRange()
{
    if (!m_thresholdSlider || !m_plot || m_frozen) {
        return;
    }

    float lo = m_plot->displayMinDb();
    float hi = m_plot->displayMaxDb();
    if (m_sampleCount > 0) {
        lo = std::min(lo, m_minPowerDb - 6.0f);
        hi = std::max(hi, m_peakPowerDb + 6.0f);
    }
    /* Always include current thr + track so auto can climb above 0 dB */
    lo = std::min(lo, m_thresholdDb - 8.0f);
    hi = std::max(hi, m_thresholdDb + 8.0f);
    if (m_trackInit) {
        lo = std::min(lo, m_trackLowDb - 4.0f);
        hi = std::max(hi, m_trackHighDb + 4.0f);
    }
    /* Was wrongly capped at 0 dB — deskHPSDR marks often sit at +10…+15 dB */
    lo = std::max(-140.0f, lo);
    hi = std::min(40.0f, hi);
    if (hi < lo + 12.0f) {
        hi = lo + 12.0f;
    }
    if (hi > 40.0f) {
        hi = 40.0f;
        lo = std::min(lo, hi - 12.0f);
    }

    const int iLo = static_cast<int>(std::floor(lo * 10.0f));
    const int iHi = static_cast<int>(std::ceil(hi * 10.0f));

    m_thresholdSlider->blockSignals(true);
    m_thresholdSlider->setRange(iLo, iHi);
    int v = static_cast<int>(std::lround(m_thresholdDb * 10.0f));
    v = std::max(iLo, std::min(iHi, v));
    m_thresholdSlider->setValue(v);
    m_thresholdSlider->blockSignals(false);

    /*
     * Only pull thr from the slider when the user is in manual mode.
     * In auto mode, never let the slider range clamp kill a higher ideal thr.
     */
    const bool autoOn = m_autoThreshCheck && m_autoThreshCheck->isChecked();
    if (!autoOn) {
        m_thresholdDb = v / 10.0f;
    }
    m_plot->setThresholdDb(m_thresholdDb);
    if (m_threshSliderLabel) {
        m_threshSliderLabel->setText(
            QString("Thresh: %1 dB [%2]")
                .arg(m_thresholdDb, 0, 'f', 1)
                .arg(autoOn ? "auto" : "manual"));
    }
}

void SignalTraceWindow::clearTrace()
{
    if (m_playbackActive) {
        stopPlayback();
    }
    if (m_frozen) {
        /* Clear while frozen would destroy hand-decode snapshot — unfreeze first */
        if (m_freezeButton) {
            m_freezeButton->setChecked(false);
        } else {
            m_frozen = false;
            if (m_plot) {
                m_plot->setFrozen(false);
            }
            applyFrozenUi(false);
        }
    }
    if (m_plot) {
        m_plot->clear();
    }
    m_lastPowerDb = -200.0f;
    m_peakPowerDb = -200.0f;
    m_minPowerDb = 0.0f;
    m_sampleCount = 0;
    m_sampleTimes.clear();
    m_capture.clear();
    updateTitleAndLabels();
}

void SignalTraceWindow::applyFrozenUi(bool frozen)
{
    if (m_freezeButton) {
        m_freezeButton->setText(frozen ? "Resume" : "Freeze");
        if (frozen) {
            m_freezeButton->setStyleSheet(
                "QPushButton { background-color: #c09000; color: black; font-weight: bold; }");
        } else {
            m_freezeButton->setStyleSheet("");
        }
    }
    /* Disable retune while frozen so the snapshot cannot be wiped accidentally */
    if (m_freqSlider) {
        m_freqSlider->setEnabled(!frozen);
    }
    if (m_binsSlider) {
        m_binsSlider->setEnabled(!frozen);
    }
    if (m_spanCombo) {
        m_spanCombo->setEnabled(!frozen);
    }
}

bool SignalTraceWindow::writeCwTrace(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&f);
    out << "# CWTRACE v1 — scope envelope capture for offline analysis\n";
    out << "format=cwtrace1\n";
    out << "abs_freq_hz=" << m_absFreqHz << "\n";
    out << "if_offset_hz=" << m_offsetHz << "\n";
    out << "base_offset_hz=" << m_baseOffsetHz << "\n";
    out << "bin_width_hz=" << m_binWidthHz << "\n";
    out << "half_bins=" << halfBins() << "\n";
    out << "threshold_db=" << m_thresholdDb << "\n";
    out << "noise_floor_db=" << m_noiseFloorDb << "\n";
    out << "sample_count=" << m_capture.size() << "\n";
    if (!m_capture.isEmpty()) {
        out << "duration_ms=" << (m_capture.last().timeMs - m_capture.first().timeMs) << "\n";
    } else {
        out << "duration_ms=0\n";
    }
    out << "# time_ms power_db above_threshold\n";
    for (const TraceSample &s : m_capture) {
        out << s.timeMs << ' ' << s.powerDb << ' ' << (s.aboveThreshold ? 1 : 0) << '\n';
    }
    return true;
}

bool SignalTraceWindow::writeKeyedWav(const QString &path) const
{
    if (m_capture.size() < 2) {
        return false;
    }

    const int sampleRate = 8000;
    const float toneHz = 700.0f;
    const qint64 t0 = m_capture.first().timeMs;
    const qint64 t1 = m_capture.last().timeMs;
    const double durationSec = std::max(0.05, (t1 - t0) / 1000.0);
    const int nSamples = static_cast<int>(std::lround(durationSec * sampleRate));
    if (nSamples <= 0 || nSamples > sampleRate * 600) {
        return false;
    }

    QVector<qint16> pcm(nSamples);
    int capIdx = 0;
    double phase = 0.0;
    const double twoPi = 2.0 * M_PI;

    for (int i = 0; i < nSamples; ++i) {
        const qint64 tMs = t0 + static_cast<qint64>((i * 1000.0) / sampleRate);
        while (capIdx + 1 < m_capture.size() && m_capture[capIdx + 1].timeMs <= tMs) {
            ++capIdx;
        }
        const bool keyed = m_capture[capIdx].aboveThreshold;
        if (keyed) {
            phase += twoPi * toneHz / sampleRate;
            if (phase > twoPi) {
                phase -= twoPi;
            }
            pcm[i] = static_cast<qint16>(std::lround(8000.0 * std::sin(phase)));
        } else {
            pcm[i] = 0;
        }
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }

    const quint32 dataBytes = static_cast<quint32>(nSamples * 2);
    const quint32 riffSize = 36 + dataBytes;
    char header[44];
    std::memset(header, 0, sizeof(header));
    std::memcpy(header + 0, "RIFF", 4);
    std::memcpy(header + 4, &riffSize, 4);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    const quint32 fmtSize = 16;
    const quint16 audioFormat = 1;
    const quint16 numChannels = 1;
    const quint32 sr = static_cast<quint32>(sampleRate);
    const quint32 byteRate = sr * 2;
    const quint16 blockAlign = 2;
    const quint16 bitsPerSample = 16;
    std::memcpy(header + 16, &fmtSize, 4);
    std::memcpy(header + 20, &audioFormat, 2);
    std::memcpy(header + 22, &numChannels, 2);
    std::memcpy(header + 24, &sr, 4);
    std::memcpy(header + 28, &byteRate, 4);
    std::memcpy(header + 32, &blockAlign, 2);
    std::memcpy(header + 34, &bitsPerSample, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &dataBytes, 4);

    if (f.write(header, 44) != 44) {
        return false;
    }
    const qint64 pcmBytes = static_cast<qint64>(nSamples) * 2;
    if (f.write(reinterpret_cast<const char *>(pcm.constData()), pcmBytes) != pcmBytes) {
        return false;
    }
    return true;
}

bool SignalTraceWindow::saveCapture(const QString &pathBase) const
{
    if (m_capture.isEmpty()) {
        return false;
    }
    const QString tracePath = pathBase + ".cwtrace";
    const QString wavPath = pathBase + ".wav";
    if (!writeCwTrace(tracePath)) {
        return false;
    }
    if (!writeKeyedWav(wavPath)) {
        return false;
    }
    return true;
}

void SignalTraceWindow::closeEvent(QCloseEvent *event)
{
    stopPlayback();
    emit closed();
    QWidget::closeEvent(event);
}

void SignalTraceWindow::freezeForHandDecode()
{
    if (m_frozen) {
        return;
    }
    if (m_freezeButton) {
        m_freezeButton->blockSignals(true);
        m_freezeButton->setChecked(true);
        m_freezeButton->blockSignals(false);
    }
    m_frozen = true;
    if (m_plot) {
        m_plot->setFrozen(true);
        m_plot->update();
    }
    applyFrozenUi(true);
    emit freezeChanged(true);
    updateTitleAndLabels();
}

void SignalTraceWindow::setStatusNotice(const QString &text)
{
    m_statusNotice = text;
    updateTitleAndLabels();
}

void SignalTraceWindow::onFreezeToggled(bool checked)
{
    m_frozen = checked;
    if (m_plot) {
        m_plot->setFrozen(checked);
    }
    applyFrozenUi(checked);
    emit freezeChanged(checked);
    updateTitleAndLabels();
    /* Force a repaint so FROZEN banner appears immediately */
    if (m_plot) {
        m_plot->update();
    }
}

void SignalTraceWindow::onClearClicked()
{
    clearTrace();
}

void SignalTraceWindow::onSpanChanged(int /*index*/)
{
    if (m_frozen) {
        return;
    }
    if (m_plot) {
        m_plot->setMaxSamples(maxSamplesForSpan());
        if (m_spanCombo) {
            m_plot->setSpanSeconds(m_spanCombo->currentData().toInt());
        }
    }
}

void SignalTraceWindow::onFreqSliderChanged(int steps)
{
    if (m_frozen) {
        return;
    }
    const float fineHz = static_cast<float>(steps) * 50.0f;
    m_offsetHz = m_baseOffsetHz + fineHz;
    m_absFreqHz = m_baseAbsHz + fineHz;
    /* Re-center AFC lock on the manual nudge */
    m_afcLockHz = m_offsetHz;
    m_afcLocked = false;
    m_afcHoldCount = 0;
    if (m_freqSliderLabel) {
        m_freqSliderLabel->setText(
            QString("Freq fine: %1%2 Hz")
                .arg(fineHz >= 0 ? "+" : "")
                .arg(fineHz, 0, 'f', 0));
    }
    m_sampleCount = 0;
    m_peakPowerDb = -200.0f;
    m_minPowerDb = 0.0f;
    if (m_plot) {
        m_plot->clear();
    }
    updateTitleAndLabels();
    emit tuningChanged();
}

void SignalTraceWindow::onBinsSliderChanged(int halfBins)
{
    if (m_frozen) {
        return;
    }
    if (m_binsSliderLabel) {
        const float bwHz = (2 * halfBins + 1) * m_binWidthHz;
        m_binsSliderLabel->setText(
            QString("Bins ±%1 (~%2 Hz)").arg(halfBins).arg(bwHz, 0, 'f', 0));
    }
    m_sampleCount = 0;
    if (m_plot) {
        m_plot->clear();
    }
    updateTitleAndLabels();
    emit tuningChanged();
}

void SignalTraceWindow::onThresholdSliderChanged(int value)
{
    /* Manual move disables auto so the slider stays where the user put it */
    if (m_autoThreshCheck && m_autoThreshCheck->isChecked()) {
        m_autoThreshCheck->blockSignals(true);
        m_autoThreshCheck->setChecked(false);
        m_autoThreshCheck->blockSignals(false);
    }
    m_thresholdDb = value / 10.0f;
    if (m_plot) {
        m_plot->setThresholdDb(m_thresholdDb);
    }
    if (m_threshSliderLabel) {
        m_threshSliderLabel->setText(QString("Thresh: %1 dB [manual]").arg(m_thresholdDb, 0, 'f', 1));
    }
    emit thresholdChanged(m_thresholdDb);
    updateTitleAndLabels();
}

void SignalTraceWindow::onAutoThreshToggled(bool checked)
{
    if (checked) {
        /* Keep history; just resume slow adaptation from current threshold */
        m_trackInit = m_powerHist.size() >= 12;
        if (m_threshSliderLabel) {
            m_threshSliderLabel->setText(
                QString("Thresh: %1 dB [auto]").arg(m_thresholdDb, 0, 'f', 1));
        }
    } else if (m_threshSliderLabel) {
        m_threshSliderLabel->setText(
            QString("Thresh: %1 dB [manual]").arg(m_thresholdDb, 0, 'f', 1));
    }
    updateTitleAndLabels();
}

void SignalTraceWindow::onOpenMorseClicked()
{
    emit openMorseRequested();
}

void SignalTraceWindow::onAfcToggled(bool checked)
{
    m_afcEnabled = checked;
    if (checked && m_hasTarget) {
        m_afcLockHz = m_offsetHz;
        m_afcLocked = false;
        m_afcHoldCount = 0;
    }
    updateTitleAndLabels();
}

void SignalTraceWindow::onAfcSearchChanged(int value)
{
    if (m_afcLabel) {
        m_afcLabel->setText(QString("AFC ±%1 Hz").arg(value * 50));
    }
    updateTitleAndLabels();
}

void SignalTraceWindow::setRecordingActive(bool on)
{
    if (on) {
        if (m_recording) {
            return;
        }
        /* Start a fresh recording — unfreeze so we capture live keying */
        if (m_frozen) {
            if (m_freezeButton) {
                m_freezeButton->blockSignals(true);
                m_freezeButton->setChecked(false);
                m_freezeButton->blockSignals(false);
            }
            m_frozen = false;
            if (m_plot) {
                m_plot->setFrozen(false);
            }
            applyFrozenUi(false);
        }
        m_recording = true;
        m_capture.clear();
        m_statusNotice.clear();
        emit recordingActiveChanged(true);
        updateTitleAndLabels();
        return;
    }

    if (!m_recording) {
        return;
    }

    /* Toggle OFF: freeze for hand-decode, then save (no modal that can hide us) */
    m_recording = false;
    emit recordingActiveChanged(false);

    /* Immediate freeze so the on-screen envelope matches the .wav */
    freezeForHandDecode();

    if (m_capture.size() >= 2) {
        emit recordingStoppedForSave();
    } else {
        m_statusNotice = "Recording stopped — not enough samples to save.";
        updateTitleAndLabels();
    }

    /* Keep this window visible and on top after stop */
    show();
    raise();
    activateWindow();
}

void SignalTraceWindow::injectPlaybackSample(float powerDb, float noiseFloorDb, qint64 sampleTimeMs)
{
    m_playbackInjecting = true;
    appendSample(powerDb, noiseFloorDb, sampleTimeMs);
    m_playbackInjecting = false;
}

bool SignalTraceWindow::loadCwTraceFile(const QString &path, QString *errorOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) {
            *errorOut = QString("Cannot open %1").arg(path);
        }
        return false;
    }

    QVector<PlaybackSample> samples;
    float absHz = m_absFreqHz;
    float ifHz = m_offsetHz;
    float baseHz = m_baseOffsetHz;
    float binW = m_binWidthHz;
    float thr = m_thresholdDb;
    float noise = m_noiseFloorDb;
    qint64 t0 = -1;

    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QChar('#'))) {
            continue;
        }
        if (line.contains(QChar('=')) && !line[0].isDigit() && !line.startsWith(QChar('-'))) {
            const int eq = line.indexOf(QChar('='));
            const QString key = line.left(eq).trimmed();
            const QString val = line.mid(eq + 1).trimmed();
            if (key == QLatin1String("abs_freq_hz")) {
                absHz = val.toFloat();
            } else if (key == QLatin1String("if_offset_hz")) {
                ifHz = val.toFloat();
            } else if (key == QLatin1String("base_offset_hz")) {
                baseHz = val.toFloat();
            } else if (key == QLatin1String("bin_width_hz")) {
                binW = val.toFloat();
            } else if (key == QLatin1String("threshold_db")) {
                thr = val.toFloat();
            } else if (key == QLatin1String("noise_floor_db")) {
                noise = val.toFloat();
            }
            continue;
        }
        const QStringList parts = line.split(QChar(' '), Qt::SkipEmptyParts);
        if (parts.size() < 2) {
            continue;
        }
        bool okT = false;
        bool okP = false;
        const qint64 t = parts[0].toLongLong(&okT);
        const float p = parts[1].toFloat(&okP);
        if (!okT || !okP || !std::isfinite(p)) {
            continue;
        }
        if (t0 < 0) {
            t0 = t;
        }
        PlaybackSample s;
        s.relTimeMs = t - t0;
        s.powerDb = p;
        samples.append(s);
    }

    if (samples.size() < 2) {
        if (errorOut) {
            *errorOut = "cwtrace has fewer than 2 samples";
        }
        return false;
    }

    m_playbackSamples = samples;
    m_playbackNoiseFloorDb = noise;
    m_binWidthHz = (binW > 1.0f) ? binW : 46.875f;
    m_baseOffsetHz = baseHz;
    m_offsetHz = ifHz;
    m_absFreqHz = absHz;
    m_hasTarget = true;
    m_thresholdDb = thr;
    m_noiseFloorDb = noise;
    m_noiseEmaDb = noise;
    if (m_plot) {
        m_plot->setThresholdDb(thr);
        m_plot->setNoiseFloor(noise);
    }
    if (m_thresholdSlider) {
        m_thresholdSlider->blockSignals(true);
        m_thresholdSlider->setValue(static_cast<int>(std::lround(thr * 10.0f)));
        m_thresholdSlider->blockSignals(false);
    }
    return true;
}

bool SignalTraceWindow::loadKeyedWavAsEnvelope(const QString &path, QString *errorOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QString("Cannot open %1").arg(path);
        }
        return false;
    }
    const QByteArray data = f.readAll();
    if (data.size() < 44) {
        if (errorOut) {
            *errorOut = "WAV too short";
        }
        return false;
    }
    if (std::memcmp(data.constData(), "RIFF", 4) != 0
        || std::memcmp(data.constData() + 8, "WAVE", 4) != 0) {
        if (errorOut) {
            *errorOut = "Not a RIFF/WAVE file";
        }
        return false;
    }

    int sampleRate = 8000;
    int bits = 16;
    int channels = 1;
    int dataOffset = -1;
    int dataBytes = 0;
    int pos = 12;
    while (pos + 8 <= data.size()) {
        const char *chunkId = data.constData() + pos;
        const quint32 chunkSize = *reinterpret_cast<const quint32 *>(data.constData() + pos + 4);
        if (std::memcmp(chunkId, "fmt ", 4) == 0 && pos + 8 + 16 <= data.size()) {
            channels = *reinterpret_cast<const quint16 *>(data.constData() + pos + 10);
            sampleRate = static_cast<int>(*reinterpret_cast<const quint32 *>(data.constData() + pos + 12));
            bits = *reinterpret_cast<const quint16 *>(data.constData() + pos + 22);
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataBytes = static_cast<int>(chunkSize);
            break;
        }
        pos += 8 + static_cast<int>(chunkSize) + (chunkSize & 1);
    }
    if (dataOffset < 0 || bits != 16 || channels < 1 || sampleRate < 1000) {
        if (errorOut) {
            *errorOut = "Unsupported WAV (need mono/stereo 16-bit PCM)";
        }
        return false;
    }
    if (dataOffset + dataBytes > data.size()) {
        dataBytes = data.size() - dataOffset;
    }
    const int nPcm = dataBytes / (2 * channels);
    if (nPcm < sampleRate / 10) {
        if (errorOut) {
            *errorOut = "WAV audio too short";
        }
        return false;
    }

    /* Envelope: RMS over ~20 ms windows → dB scale matching scope captures */
    const int win = std::max(1, sampleRate / 50);
    QVector<PlaybackSample> samples;
    samples.reserve(nPcm / win + 1);
    const qint16 *pcm = reinterpret_cast<const qint16 *>(data.constData() + dataOffset);
    for (int i = 0; i + win <= nPcm; i += win) {
        double acc = 0.0;
        for (int j = 0; j < win; ++j) {
            const double s = pcm[(i + j) * channels] / 32768.0;
            acc += s * s;
        }
        const double rms = std::sqrt(acc / win);
        /* Keyed capture uses ~0.24 peak → map silence→~-80, tone→~-52 */
        float powerDb;
        if (rms < 1e-4) {
            powerDb = -80.0f;
        } else {
            powerDb = static_cast<float>(20.0 * std::log10(rms) - 20.0);
            powerDb = std::max(-90.0f, std::min(-40.0f, powerDb));
        }
        PlaybackSample ps;
        ps.relTimeMs = static_cast<qint64>((i * 1000.0) / sampleRate);
        ps.powerDb = powerDb;
        samples.append(ps);
    }
    if (samples.size() < 2) {
        if (errorOut) {
            *errorOut = "Could not build envelope from WAV";
        }
        return false;
    }

    m_playbackSamples = samples;
    m_playbackNoiseFloorDb = -80.0f;
    if (!m_hasTarget) {
        m_hasTarget = true;
        m_baseOffsetHz = 0.0f;
        m_offsetHz = 0.0f;
        m_absFreqHz = 0.0f;
        m_binWidthHz = 46.875f;
    }
    m_noiseFloorDb = -80.0f;
    m_noiseEmaDb = -80.0f;
    m_thresholdDb = -66.0f;
    if (m_plot) {
        m_plot->setNoiseFloor(-80.0f);
        m_plot->setThresholdDb(m_thresholdDb);
    }
    return true;
}

bool SignalTraceWindow::startPlayback(const QString &path)
{
    stopPlayback();

    QString err;
    QString loadPath = path;
    const QFileInfo fi(path);
    const QString suffix = fi.suffix().toLower();

    /*
     * Prefer .cwtrace: it has real power_db samples. A .wav is only the keyed
     * 700 Hz tone — fine for ears, lossy for the decoder envelope.
     */
    if (suffix == QLatin1String("wav")) {
        const QString sibling = fi.path() + QChar('/') + fi.completeBaseName() + ".cwtrace";
        if (QFileInfo::exists(sibling)) {
            loadPath = sibling;
        }
    }

    bool ok = false;
    if (loadPath.endsWith(QLatin1String(".cwtrace"), Qt::CaseInsensitive)) {
        ok = loadCwTraceFile(loadPath, &err);
    } else if (loadPath.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive)) {
        ok = loadKeyedWavAsEnvelope(loadPath, &err);
    } else {
        err = "Choose a .cwtrace or .wav file";
    }
    if (!ok) {
        m_statusNotice = QString("Playback load failed: %1").arg(err);
        updateTitleAndLabels();
        return false;
    }

    /* Unfreeze and clear scope; block live RF via m_playbackActive */
    if (m_frozen) {
        if (m_freezeButton) {
            m_freezeButton->blockSignals(true);
            m_freezeButton->setChecked(false);
            m_freezeButton->blockSignals(false);
        }
        m_frozen = false;
        if (m_plot) {
            m_plot->setFrozen(false);
        }
        applyFrozenUi(false);
    }
    if (m_recording) {
        m_recording = false;
        emit recordingActiveChanged(false);
    }

    if (m_plot) {
        m_plot->clear();
    }
    m_capture.clear();
    m_powerHist.clear();
    m_sampleTimes.clear();
    m_sampleCount = 0;
    m_keyIsHigh = false;
    m_keyPendingHigh = false;
    m_keyEdgeStartMs = -1;
    m_lastDetectTimeMs = -1;
    m_trackInit = false;
    m_peakPowerDb = -200.0f;
    m_minPowerDb = 0.0f;
    m_lastPowerDb = -200.0f;
    m_noiseAvgDb = -200.0f;
    m_noisePeakDb = -200.0f;
    m_markPeakHoldDb = -200.0f;
    m_markBodyDb = -200.0f;
    if (m_plot) {
        m_plot->setMarkPeakDb(-200.0f);
    }

    /* Expand span if capture is longer than current window */
    if (m_spanCombo && !m_playbackSamples.isEmpty()) {
        const qint64 durMs = m_playbackSamples.last().relTimeMs;
        const int needSec = static_cast<int>((durMs / 1000) + 2);
        int bestIdx = m_spanCombo->currentIndex();
        for (int i = 0; i < m_spanCombo->count(); ++i) {
            if (m_spanCombo->itemData(i).toInt() >= needSec) {
                bestIdx = i;
                break;
            }
            bestIdx = i;  /* take longest if still short */
        }
        m_spanCombo->blockSignals(true);
        m_spanCombo->setCurrentIndex(bestIdx);
        m_spanCombo->blockSignals(false);
        if (m_plot) {
            const int span = m_spanCombo->currentData().toInt();
            m_plot->setSpanSeconds(span);
            m_plot->setMaxSamples(maxSamplesForSpan());
        }
    }

    m_playbackPath = path;
    m_playbackIndex = 0;
    m_playbackActive = true;
    m_playbackWallStartMs = m_clock.elapsed();
    if (m_playButton) {
        m_playButton->setEnabled(false);
    }
    if (m_stopPlayButton) {
        m_stopPlayButton->setEnabled(true);
    }

    if (m_autoThreshCheck && m_autoThreshCheck->isChecked() && m_threshSliderLabel) {
        m_threshSliderLabel->setText(
            QString("Thresh: %1 dB [auto/play]").arg(m_thresholdDb, 0, 'f', 1));
    }

    const bool autoOn = m_autoThreshCheck && m_autoThreshCheck->isChecked();
    m_statusNotice = QString("PLAYBACK %1 (%2 samples, %3 s) thr=%4 dB %5 — live RF ignored")
                         .arg(QFileInfo(loadPath).fileName())
                         .arg(m_playbackSamples.size())
                         .arg(m_playbackSamples.last().relTimeMs / 1000.0, 0, 'f', 1)
                         .arg(m_thresholdDb, 0, 'f', 1)
                         .arg(autoOn ? QStringLiteral("auto") : QStringLiteral("manual/frozen"));
    updateTitleAndLabels();
    emit thresholdChanged(m_thresholdDb);
    /*
     * Notify parent first so Morse prepareForPlayback() / show() run before
     * any samples are injected (otherwise samples are dropped or squelched).
     */
    emit playbackStarted(loadPath);
    /* Let the dialog map / become visible before the first decode samples */
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    if (m_playbackTimer) {
        m_playbackTimer->start();
    }
    /* Push first samples after Morse is ready */
    onPlaybackTick();
    return true;
}

void SignalTraceWindow::stopPlayback()
{
    if (m_playbackTimer) {
        m_playbackTimer->stop();
    }
    const bool wasActive = m_playbackActive;
    m_playbackActive = false;
    m_playbackInjecting = false;
    m_playbackIndex = 0;
    m_playbackSamples.clear();
    if (m_playButton) {
        m_playButton->setEnabled(true);
    }
    if (m_stopPlayButton) {
        m_stopPlayButton->setEnabled(false);
    }
    if (wasActive) {
        m_statusNotice = "Playback stopped.";
        updateTitleAndLabels();
        emit playbackFinished();
    }
}

void SignalTraceWindow::onPlayCaptureClicked()
{
    QString startDir;
    QDir base(QCoreApplication::applicationDirPath());
    if (base.dirName() == QLatin1String("bin")) {
        base.cdUp();
    }
    const QString captures = base.filePath(QStringLiteral("captures"));
    startDir = QDir(captures).exists() ? captures : QDir::homePath();

    const QString path = QFileDialog::getOpenFileName(
        this,
        "Play capture as live signal",
        startDir,
        "Captures (*.wav *.cwtrace);;WAV (*.wav);;CWTRACE (*.cwtrace);;All (*)");
    if (path.isEmpty()) {
        return;
    }
    if (!startPlayback(path)) {
        show();
        raise();
    }
}

void SignalTraceWindow::onStopPlaybackClicked()
{
    stopPlayback();
}

void SignalTraceWindow::onPlaybackTick()
{
    if (!m_playbackActive || m_playbackSamples.isEmpty()) {
        return;
    }

    const qint64 wallMs = m_clock.elapsed() - m_playbackWallStartMs;
    const double speed = (m_playbackSpeed > 0.05) ? m_playbackSpeed : 1.0;
    const qint64 playMs = static_cast<qint64>(wallMs * speed);

    int fed = 0;
    while (m_playbackIndex < m_playbackSamples.size()) {
        const PlaybackSample &s = m_playbackSamples[m_playbackIndex];
        if (s.relTimeMs > playMs) {
            break;
        }
        /* Timestamps relative to playback wall start → correct CW element lengths */
        const qint64 tMs = m_playbackWallStartMs + s.relTimeMs;
        injectPlaybackSample(s.powerDb, m_playbackNoiseFloorDb, tMs);
        ++m_playbackIndex;
        ++fed;
        /* Avoid UI stall if timestamps bunch up */
        if (fed >= 40) {
            break;
        }
    }

    if (m_playbackIndex >= m_playbackSamples.size()) {
        if (m_playbackTimer) {
            m_playbackTimer->stop();
        }
        m_playbackActive = false;
        if (m_playButton) {
            m_playButton->setEnabled(true);
        }
        if (m_stopPlayButton) {
            m_stopPlayButton->setEnabled(false);
        }
        m_statusNotice = QString("Playback finished: %1")
                             .arg(QFileInfo(m_playbackPath).fileName());
        m_playbackSamples.clear();
        updateTitleAndLabels();
        emit playbackFinished();
    } else if (fed > 0) {
        updateTitleAndLabels();
    }
}

void SignalTraceWindow::updateTitleAndLabels()
{
    if (!m_hasTarget) {
        setWindowTitle("CW Signal Trace");
        if (m_freqLabel) {
            m_freqLabel->setText("No signal selected — click the waterfall");
        }
        if (m_statsLabel) {
            m_statsLabel->clear();
            m_statsLabel->hide();
        }
        return;
    }

    const qint64 roundedHz = static_cast<qint64>(std::llround(m_absFreqHz));
    const qint64 mhz = roundedHz / 1000000;
    const qint64 khz = (roundedHz / 1000) % 1000;
    const qint64 hz = roundedHz % 1000;

    /* RF frequency only — no IF / thr / SNR / AFC status clutter */
    const QString freqText =
        QString("RF %1.%2.%3 MHz")
            .arg(mhz)
            .arg(khz, 3, 10, QChar('0'))
            .arg(hz, 3, 10, QChar('0'));

    setWindowTitle(QString("CW Signal Trace — %1.%2 MHz")
                       .arg(mhz)
                       .arg(khz, 3, 10, QChar('0')));

    if (m_freqLabel) {
        m_freqLabel->setText(freqText);
    }

    const int hb = halfBins();
    const float bwHz = (2 * hb + 1) * m_binWidthHz;
    if (m_binsSliderLabel) {
        m_binsSliderLabel->setText(
            QString("Bins ±%1 (~%2 Hz)").arg(hb).arg(bwHz, 0, 'f', 0));
    }

    if (m_statsLabel) {
        m_statsLabel->clear();
        m_statsLabel->hide();
    }
}
