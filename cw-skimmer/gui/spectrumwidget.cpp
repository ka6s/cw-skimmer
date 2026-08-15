/**
 * @file spectrumwidget.cpp
 * @brief Horizontal scrolling spectrum waterfall display
 */

#include "spectrumwidget.h"
#include <QPainter>
#include <QResizeEvent>
#include <QDateTime>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <QElapsedTimer>

namespace {

int columnDisplayX(int colIndex, int totalCols, int plotWidth)
{
    if (totalCols <= 0 || colIndex < 0 || colIndex >= totalCols || plotWidth <= 0) {
        return -1;
    }

    const int firstVisible = std::max(0, totalCols - plotWidth);
    if (colIndex < firstVisible) {
        return -1;
    }

    if (totalCols <= plotWidth) {
        return plotWidth - totalCols + colIndex;
    }

    return colIndex - firstVisible;
}

}  // namespace

SpectrumWidget::SpectrumWidget(QWidget *parent)
    : QWidget(parent)
    , m_plotWidth(0)
    , m_plotHeight(0)
    , m_noiseFloorDb(-95.0f)
    , m_centerFrequency(0)
    , m_binWidth(48000.0f / 1024.0f)
    , m_maxSignalAge(10000)
    , m_scrollWidth(800)
    , m_hasTraceSelection(false)
    , m_traceOffsetHz(0.0f)
    , m_whiteStreak(0)
    , m_whiteEventCount(0)
    , m_lastWhiteLogMs(0)
    , m_lastGoodNoiseDb(-95.0f)
    , m_perfUpdateCount(0)
    , m_perfUpdateTotalMs(0.0)
    , m_perfUpdateMaxMs(0.0)
    , m_perfScrollTotalMs(0.0)
    , m_perfDrawColTotalMs(0.0)
    , m_perfPaintTotalMs(0.0)
    , m_perfPaintMaxMs(0.0)
    , m_perfPaintCount(0)
{
    setMinimumHeight(300);
    setStyleSheet("background-color: black;");
    setMouseTracking(true);
    setToolTip("Click a signal: open single-signal strength scope\n"
               "Shift+click: freeze capture for offline decode testing");
}

void SpectrumWidget::clearTraceSelection()
{
    m_hasTraceSelection = false;
    m_traceOffsetHz = 0.0f;
    update();
}

void SpectrumWidget::setTraceOffsetHz(float offsetHz)
{
    m_hasTraceSelection = true;
    m_traceOffsetHz = offsetHz;
    update();
}

float SpectrumWidget::powerAtOffset(const QVector<float> &spectrum, float freqOffsetHz,
                                    float binWidth, int halfBins)
{
    if (spectrum.isEmpty() || binWidth <= 0.0f) {
        return -200.0f;
    }

    const int numBins = spectrum.size();
    int centerBin = numBins / 2 + static_cast<int>(std::lround(freqOffsetHz / binWidth));
    centerBin = std::max(0, std::min(numBins - 1, centerBin));
    const int hb = std::max(0, halfBins);

    float best = -200.0f;
    for (int b = centerBin - hb; b <= centerBin + hb; ++b) {
        if (b < 0 || b >= numBins) {
            continue;
        }
        best = std::max(best, spectrum[b]);
    }
    return best;
}

float SpectrumWidget::peakOffsetNear(const QVector<float> &spectrum, float centerOffsetHz,
                                     float binWidth, float searchHalfHz,
                                     float *powerDbOut, float noiseFloorDb,
                                     float *snrDbOut)
{
    if (spectrum.isEmpty() || binWidth <= 0.0f) {
        if (powerDbOut) {
            *powerDbOut = -200.0f;
        }
        if (snrDbOut) {
            *snrDbOut = -200.0f;
        }
        return centerOffsetHz;
    }

    const int numBins = spectrum.size();
    const int centerBin = numBins / 2 + static_cast<int>(std::lround(centerOffsetHz / binWidth));
    const int searchBins = std::max(1, static_cast<int>(std::lround(searchHalfHz / binWidth)));

    int bestBin = centerBin;
    float bestPower = -200.0f;
    for (int b = centerBin - searchBins; b <= centerBin + searchBins; ++b) {
        if (b < 1 || b >= numBins - 1) {
            continue;
        }
        if (spectrum[b] > bestPower) {
            bestPower = spectrum[b];
            bestBin = b;
        }
    }

    /* Parabolic interpolation around peak for sub-bin accuracy */
    float delta = 0.0f;
    if (bestBin > 0 && bestBin < numBins - 1) {
        const float y1 = spectrum[bestBin - 1];
        const float y2 = spectrum[bestBin];
        const float y3 = spectrum[bestBin + 1];
        const float denom = (y1 - 2.0f * y2 + y3);
        if (std::fabs(denom) > 1e-6f) {
            delta = 0.5f * (y1 - y3) / denom;
            delta = std::max(-0.5f, std::min(0.5f, delta));
        }
    }

    const float peakOffset =
        (static_cast<float>(bestBin) + delta - static_cast<float>(numBins) * 0.5f) * binWidth;

    if (powerDbOut) {
        *powerDbOut = bestPower;
    }
    if (snrDbOut) {
        *snrDbOut = bestPower - noiseFloorDb;
    }
    return peakOffset;
}

QVector<float> SpectrumWidget::powerHistoryAtOffset(float freqOffsetHz, int halfBins) const
{
    QVector<float> history;
    history.reserve(m_waterfallColumns.size());
    for (const QVector<float> &col : m_waterfallColumns) {
        history.append(powerAtOffset(col, freqOffsetHz, m_binWidth, halfBins));
    }
    return history;
}

float SpectrumWidget::latestNoiseFloorDb() const
{
    if (m_waterfallColumns.isEmpty()) {
        return m_noiseFloorDb;
    }
    return estimateNoiseFloor(m_waterfallColumns.last());
}

int SpectrumWidget::offsetToBinIndex(float freqOffsetHz, int numBins) const
{
    if (numBins <= 0 || m_binWidth <= 0.0f) {
        return numBins / 2;
    }

    const int binIndex = numBins / 2 + static_cast<int>(std::lround(freqOffsetHz / m_binWidth));
    return std::max(0, std::min(numBins - 1, binIndex));
}

int SpectrumWidget::binIndexToY(int binIndex, int numBins, int plotY, int plotHeight) const
{
    if (numBins <= 1) {
        return plotY + plotHeight / 2;
    }

    const float yf = plotY +
        ((numBins - 1 - binIndex) / static_cast<float>(numBins - 1)) * plotHeight;
    return static_cast<int>(yf);
}

QRect SpectrumWidget::plotRect() const
{
    const int plotX = 40;
    const int plotY = 20;
    const int plotWidth = std::max(1, width() - 60);
    const int plotHeight = std::max(1, height() - 60);
    return QRect(plotX, plotY, plotWidth, plotHeight);
}

float SpectrumWidget::yToFreqOffsetHz(int y) const
{
    const QRect plot = plotRect();
    const int plotY = plot.y();
    const int plotHeight = plot.height();
    const int numBins = m_waterfallColumns.isEmpty() ? 1024 : m_waterfallColumns.last().size();

    if (numBins <= 1 || plotHeight <= 0 || m_binWidth <= 0.0f) {
        return 0.0f;
    }

    const float frac = 1.0f - (y - plotY) / static_cast<float>(plotHeight);
    int binIndex = static_cast<int>(std::lround(frac * (numBins - 1)));
    binIndex = std::max(0, std::min(numBins - 1, binIndex));
    return (binIndex - numBins / 2) * m_binWidth;
}

void SpectrumWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton &&
        m_binWidth > 0.0f &&
        !m_waterfallColumns.isEmpty()) {
        const QRect plot = plotRect();
        if (!plot.contains(event->pos()) &&
            (event->pos().y() < plot.top() || event->pos().y() > plot.bottom())) {
            QWidget::mousePressEvent(event);
            return;
        }

        const int y = std::max(plot.top(), std::min(plot.bottom(), event->pos().y()));
        const float offsetHz = snapOffsetHz(yToFreqOffsetHz(y));
        const float absFreqHz = m_centerFrequency + offsetHz;

        if (event->modifiers() & Qt::ShiftModifier) {
            fprintf(stderr, "[CAPTURE-GUI] shift-click y=%d offset=%.1f Hz abs=%.0f Hz\n",
                    event->pos().y(), offsetHz, absFreqHz);
            fflush(stderr);
            emit captureMarkRequested(offsetHz, absFreqHz);
        } else {
            m_hasTraceSelection = true;
            m_traceOffsetHz = offsetHz;
            fprintf(stderr, "[TRACE-GUI] click y=%d offset=%.1f Hz abs=%.0f Hz\n",
                    event->pos().y(), offsetHz, absFreqHz);
            fflush(stderr);
            emit signalTraceRequested(offsetHz, absFreqHz);
            update();
        }
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

float SpectrumWidget::snapOffsetHz(float freqOffsetHz) const
{
    if (m_waterfallColumns.isEmpty() || m_binWidth <= 0.0f) {
        return freqOffsetHz;
    }

    const QVector<float> &spectrum = m_waterfallColumns.last();
    const int numBins = spectrum.size();
    if (numBins <= 2) {
        return freqOffsetHz;
    }

    int centerBin = numBins / 2 + static_cast<int>(std::lround(freqOffsetHz / m_binWidth));
    const int search = 12;
    int bestBin = centerBin;
    float bestPower = -200.0f;

    for (int b = centerBin - search; b <= centerBin + search; ++b) {
        if (b < 1 || b >= numBins - 1) {
            continue;
        }
        if (spectrum[b] > bestPower) {
            bestPower = spectrum[b];
            bestBin = b;
        }
    }

    return (static_cast<float>(bestBin) - static_cast<float>(numBins) * 0.5f) * m_binWidth;
}

void SpectrumWidget::addSignal(float frequency, float freqOffsetHz, float snr)
{
    const long now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_signals.size() - 1; i >= 0; --i) {
        if (now - m_signals[i].timestamp > m_maxSignalAge) {
            m_signals.removeAt(i);
        }
    }

    const float snappedOffset = snapOffsetHz(freqOffsetHz);

    /* One marker per ~50 Hz bucket — match on IF offset, not absolute RF */
    for (int i = 0; i < m_signals.size(); ++i) {
        if (std::fabs(m_signals[i].freqOffsetHz - snappedOffset) < 50.0f) {
            m_signals[i].frequency = frequency;
            m_signals[i].freqOffsetHz = snappedOffset;
            m_signals[i].snr = snr;
            m_signals[i].timestamp = now;
            update();
            return;
        }
    }

    SignalData signal;
    signal.frequency = frequency;
    signal.freqOffsetHz = snappedOffset;
    signal.snr = snr;
    signal.timestamp = now;
    m_signals.append(signal);
    update();
}

float SpectrumWidget::estimateNoiseFloor(const QVector<float> &spectrum) const
{
    if (spectrum.isEmpty()) {
        return m_noiseFloorDb;
    }

    QVector<float> sorted;
    sorted.reserve(spectrum.size());
    const int n = spectrum.size();
    const int dc = n / 2;
    const int guard = std::max(2, n / 64);
    /* Also skip band edges (IQ DC at centre; real-audio LF at bin 0) */
    const int edge = std::max(2, n / 32);

    for (int i = 0; i < n; ++i) {
        if (i < edge || i >= n - edge) {
            continue;
        }
        if (i >= dc - guard && i <= dc + guard) {
            continue;
        }
        sorted.append(spectrum[i]);
    }

    if (sorted.isEmpty()) {
        for (float v : spectrum) {
            sorted.append(v);
        }
    }
    if (sorted.isEmpty()) {
        return m_noiseFloorDb;
    }

    std::sort(sorted.begin(), sorted.end());
    /* Slightly higher percentile so radio passband noise doesn't map all white */
    const int idx = std::min(sorted.size() - 1, sorted.size() * 2 / 5);
    return sorted[idx];
}

void SpectrumWidget::ensureWaterfallImage(int plotWidth, int plotHeight)
{
    if (plotWidth <= 0 || plotHeight <= 0) {
        return;
    }

    m_plotWidth = plotWidth;
    m_plotHeight = plotHeight;

    if (m_waterfallImage.width() == plotWidth && m_waterfallImage.height() == plotHeight) {
        return;
    }

    m_waterfallImage = QImage(plotWidth, plotHeight, QImage::Format_RGB32);
    rebuildWaterfallImage();
}

void SpectrumWidget::rebuildWaterfallImage()
{
    if (m_waterfallImage.isNull() || m_plotWidth <= 0 || m_plotHeight <= 0) {
        return;
    }

    m_waterfallImage.fill(Qt::black);

    const int numCols = m_waterfallColumns.size();
    for (int col = 0; col < numCols; ++col) {
        const int x = columnDisplayX(col, numCols, m_plotWidth);
        if (x < 0) {
            continue;
        }
        const QVector<float> &spectrum = m_waterfallColumns[col];
        const float noise = estimateNoiseFloor(spectrum);
        drawWaterfallColumn(x, spectrum, noise);
    }
}

void SpectrumWidget::drawWaterfallColumn(int x, const QVector<float> &spectrum, float noise_floor_db)
{
    if (x < 0 || x >= m_plotWidth || spectrum.isEmpty() || m_waterfallImage.isNull()) {
        return;
    }

    const int numBins = spectrum.size();

    for (int disp_row = 0; disp_row < m_plotHeight; ++disp_row) {
        const float frac0 = static_cast<float>(disp_row) / m_plotHeight;
        const float frac1 = static_cast<float>(disp_row + 1) / m_plotHeight;
        int bin_hi = static_cast<int>((1.0f - frac0) * numBins);
        int bin_lo = static_cast<int>((1.0f - frac1) * numBins);
        bin_hi = std::max(0, std::min(numBins - 1, bin_hi));
        bin_lo = std::max(0, std::min(numBins - 1, bin_lo));
        if (bin_lo > bin_hi) {
            std::swap(bin_lo, bin_hi);
        }

        float max_power = -200.0f;
        for (int b = bin_lo; b <= bin_hi; ++b) {
            max_power = std::max(max_power, spectrum[b]);
        }

        QRgb *scanLine = reinterpret_cast<QRgb *>(m_waterfallImage.scanLine(disp_row));
        scanLine[x] = powerToColor(max_power, noise_floor_db).rgb();
    }
}

QColor SpectrumWidget::powerToColor(float power_db, float noise_floor_db) const
{
    if (!std::isfinite(power_db)) {
        return Qt::black;
    }
    const float floor = std::max(-110.0f, noise_floor_db);
    /* Higher threshold + wider range → passband is grey, CW peaks stand out white */
    const float threshold = floor + 8.0f;
    if (power_db < threshold) {
        return Qt::black;
    }

    float t = (power_db - threshold) / 35.0f;
    t = std::max(0.0f, std::min(1.0f, t));
    /* Mild gamma so mid levels stay readable without blowing to pure white */
    t = std::pow(t, 0.85f);
    const int v = static_cast<int>(t * 255.0f + 0.5f);
    return QColor(v, v, v);
}

SpectrumWidget::SpectrumStats SpectrumWidget::analyzeSpectrum(
    const QVector<float> &spectrum, float noiseFloorDb) const
{
    SpectrumStats st;
    st.numBins = spectrum.size();
    if (spectrum.isEmpty()) {
        st.badData = true;
        return st;
    }

    float sum = 0.0f;
    st.minDb = 1.0e9f;
    st.maxDb = -1.0e9f;
    QVector<float> finite;
    finite.reserve(spectrum.size());

    for (float v : spectrum) {
        if (!std::isfinite(v)) {
            if (std::isnan(v)) {
                st.nanCount++;
            } else {
                st.infCount++;
            }
            continue;
        }
        st.finiteCount++;
        finite.append(v);
        sum += v;
        st.minDb = std::min(st.minDb, v);
        st.maxDb = std::max(st.maxDb, v);
    }

    if (finite.isEmpty()) {
        st.badData = true;
        st.minDb = st.maxDb = st.meanDb = st.medianDb = -200.0f;
        st.noiseDb = noiseFloorDb;
        return st;
    }

    st.meanDb = sum / static_cast<float>(finite.size());
    std::sort(finite.begin(), finite.end());
    st.medianDb = finite[finite.size() / 2];
    st.noiseDb = noiseFloorDb;
    st.dynamicRangeDb = st.maxDb - st.minDb;

    /* Fraction of bins that would paint near-white under current color map */
    const float floor = std::max(-110.0f, noiseFloorDb);
    const float whiteThr = floor + 8.0f + 0.90f * 35.0f; /* t >= 0.9 → near white */
    int hot = 0;
    for (float v : finite) {
        if (v >= whiteThr) {
            hot++;
        }
    }
    st.hotFrac = static_cast<float>(hot) / static_cast<float>(finite.size());

    /*
     * All-white signatures:
     *  - most bins above white threshold (passband blown out)
     *  - tiny dynamic range but elevated (flat saturated spectrum)
     *  - noise floor far below median (noise estimate collapsed)
     */
    const bool saturated = st.hotFrac >= 0.85f;
    const bool flatHot = (st.dynamicRangeDb < 6.0f && st.medianDb > floor + 20.0f);
    const bool noiseCollapse = (st.medianDb - floor) > 40.0f && st.hotFrac >= 0.50f;
    st.allWhite = saturated || flatHot || noiseCollapse;
    st.allBlack = (st.maxDb < floor + 8.0f);
    st.badData = (st.nanCount + st.infCount) > spectrum.size() / 4;
    return st;
}

void SpectrumWidget::handleWhiteWaterfall(const SpectrumStats &st,
                                          const QVector<float> &spectrum)
{
    m_whiteStreak++;
    m_whiteEventCount++;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    /* Log on first white column of a streak, then every ~2 s while it continues */
    const bool shouldLog = (m_whiteStreak == 1)
                           || (nowMs - m_lastWhiteLogMs >= 2000)
                           || (m_whiteStreak == 5)
                           || (m_whiteStreak == 30);
    if (!shouldLog) {
        return;
    }
    m_lastWhiteLogMs = nowMs;

    const QString msg = QStringLiteral(
        "WATERFALL WHITE/ANOMALY  streak=%1 event#=%2 bins=%3 finite=%4 nan=%5 inf=%6  "
        "min=%7 max=%8 mean=%9 med=%10 noise=%11 hot%%=%12 range=%13  "
        "center=%14 binW=%15 lastGoodNoise=%16  (stop/start often recovers)")
                            .arg(m_whiteStreak)
                            .arg(m_whiteEventCount)
                            .arg(st.numBins)
                            .arg(st.finiteCount)
                            .arg(st.nanCount)
                            .arg(st.infCount)
                            .arg(st.minDb, 0, 'f', 1)
                            .arg(st.maxDb, 0, 'f', 1)
                            .arg(st.meanDb, 0, 'f', 1)
                            .arg(st.medianDb, 0, 'f', 1)
                            .arg(st.noiseDb, 0, 'f', 1)
                            .arg(st.hotFrac * 100.0f, 0, 'f', 0)
                            .arg(st.dynamicRangeDb, 0, 'f', 1)
                            .arg(m_centerFrequency, 0, 'f', 0)
                            .arg(m_binWidth, 0, 'f', 3)
                            .arg(m_lastGoodNoiseDb, 0, 'f', 1);
    emit waterfallAnomaly(msg);

    /* Extra sample of first few bins for freeze forensics (first streak only) */
    if (m_whiteStreak == 1 && !spectrum.isEmpty()) {
        QString head = QStringLiteral("WATERFALL WHITE sample[0..7]=");
        for (int i = 0; i < std::min(8, spectrum.size()); ++i) {
            head += QString::number(spectrum[i], 'f', 1);
            if (i + 1 < std::min(8, spectrum.size())) {
                head += QLatin1Char(',');
            }
        }
        emit waterfallAnomaly(head);
    }
}

void SpectrumWidget::scrollWaterfallLeft()
{
    if (m_plotWidth <= 1 || m_waterfallImage.isNull()) {
        return;
    }

    const int shiftBytes = (m_plotWidth - 1) * static_cast<int>(sizeof(QRgb));
    for (int row = 0; row < m_plotHeight; ++row) {
        QRgb *line = reinterpret_cast<QRgb *>(m_waterfallImage.scanLine(row));
        std::memmove(line, line + 1, static_cast<size_t>(shiftBytes));
        line[m_plotWidth - 1] = qRgb(0, 0, 0);
    }
}

void SpectrumWidget::appendWaterfallColumn(const QVector<float> &powerSpectrum)
{
    if (powerSpectrum.isEmpty()) {
        return;
    }

    /* Sanitize non-finite bins so they cannot blow the color map / UI */
    QVector<float> clean = powerSpectrum;
    int fixed = 0;
    for (int i = 0; i < clean.size(); ++i) {
        if (!std::isfinite(clean[i])) {
            clean[i] = m_lastGoodNoiseDb;
            fixed++;
        }
    }

    float noise = estimateNoiseFloor(clean);
    SpectrumStats st = analyzeSpectrum(clean, noise);

    if (st.allWhite || st.badData) {
        handleWhiteWaterfall(st, clean);
        /*
         * Soft recovery: re-map with a raised floor so the column is grey not
         * solid white, and prefer last good column when data is fully invalid.
         * This avoids painting a white sheet that often coincides with freezes.
         */
        if (st.badData && !m_lastGoodColumn.isEmpty()
            && m_lastGoodColumn.size() == clean.size()) {
            clean = m_lastGoodColumn;
            noise = m_lastGoodNoiseDb;
            st = analyzeSpectrum(clean, noise);
        } else {
            /* Raise effective floor toward median so hotFrac collapses */
            const float rescueFloor = std::max(noise, st.medianDb - 12.0f);
            noise = rescueFloor;
            m_noiseFloorDb = rescueFloor;
        }
    } else {
        if (m_whiteStreak > 0) {
            emit waterfallAnomaly(
                QStringLiteral("WATERFALL recovered after %1 white columns (events total %2)")
                    .arg(m_whiteStreak)
                    .arg(m_whiteEventCount));
        }
        m_whiteStreak = 0;
        m_lastGoodNoiseDb = noise;
        m_lastGoodColumn = clean;
        m_noiseFloorDb = noise;
    }

    m_waterfallColumns.append(clean);
    while (m_waterfallColumns.size() > m_scrollWidth) {
        m_waterfallColumns.removeFirst();
    }

    if (m_waterfallImage.isNull() || m_plotWidth <= 0 || m_plotHeight <= 0) {
        return;
    }

    QElapsedTimer scrollTimer;
    scrollTimer.start();
    scrollWaterfallLeft();
    m_perfScrollTotalMs += scrollTimer.nsecsElapsed() / 1e6;

    QElapsedTimer drawTimer;
    drawTimer.start();
    drawWaterfallColumn(m_plotWidth - 1, clean, noise);
    m_perfDrawColTotalMs += drawTimer.nsecsElapsed() / 1e6;
}

void SpectrumWidget::updateSpectrum(const QVector<float> &powerSpectrum, float centerFreq, float binWidth)
{
    QElapsedTimer timer;
    timer.start();

    m_centerFrequency = centerFreq;
    m_binWidth = binWidth;

    if (powerSpectrum.isEmpty()) {
        return;
    }

    const int plotWidth = std::max(1, width() - 60);
    const int plotHeight = std::max(1, height() - 60);
    const bool sizeChanged =
        (plotWidth != m_plotWidth) || (plotHeight != m_plotHeight) || m_waterfallImage.isNull();

    if (sizeChanged) {
        /* Route through append path for white detection + sanitize */
        m_plotWidth = 0; /* force ensureWaterfallImage after stats */
        ensureWaterfallImage(plotWidth, plotHeight);
        appendWaterfallColumn(powerSpectrum);
        rebuildWaterfallImage();
        update();
        return;
    }

    appendWaterfallColumn(powerSpectrum);

    const double elapsedMs = timer.nsecsElapsed() / 1e6;
    m_perfUpdateCount++;
    m_perfUpdateTotalMs += elapsedMs;
    if (elapsedMs > m_perfUpdateMaxMs) {
        m_perfUpdateMaxMs = elapsedMs;
    }
    if (m_perfUpdateCount % 100 == 0) {
        fprintf(stderr,
                "[PERF-GUI] waterfall_update: count=%lld avg=%.2f ms max=%.2f ms "
                "scroll_avg=%.2f ms draw_col_avg=%.2f ms paint_avg=%.2f ms paint_max=%.2f ms "
                "plot=%dx%d cols=%d\n",
                static_cast<long long>(m_perfUpdateCount),
                m_perfUpdateTotalMs / m_perfUpdateCount,
                m_perfUpdateMaxMs,
                m_perfScrollTotalMs / m_perfUpdateCount,
                m_perfDrawColTotalMs / m_perfUpdateCount,
                m_perfPaintCount > 0 ? m_perfPaintTotalMs / m_perfPaintCount : 0.0,
                m_perfPaintMaxMs,
                m_plotWidth, m_plotHeight, m_waterfallColumns.size());
        fflush(stderr);
    }

    update();
}

void SpectrumWidget::appendSpectrumColumns(const QVector<QVector<float>> &columns,
                                           float centerFreq, float binWidth)
{
    if (columns.isEmpty()) {
        return;
    }

    m_centerFrequency = centerFreq;
    m_binWidth = binWidth;

    const int plotWidth = std::max(1, width() - 60);
    const int plotHeight = std::max(1, height() - 60);
    const bool sizeChanged =
        (plotWidth != m_plotWidth) || (plotHeight != m_plotHeight) || m_waterfallImage.isNull();

    if (sizeChanged) {
        ensureWaterfallImage(plotWidth, plotHeight);
        for (const QVector<float> &col : columns) {
            if (!col.isEmpty()) {
                appendWaterfallColumn(col); /* includes white detection + sanitize */
            }
        }
        rebuildWaterfallImage();
        update();
        return;
    }

    for (const QVector<float> &col : columns) {
        if (!col.isEmpty()) {
            appendWaterfallColumn(col);
        }
    }

    update();
}

void SpectrumWidget::clear()
{
    m_signals.clear();
    m_waterfallColumns.clear();
    m_hasTraceSelection = false;
    m_traceOffsetHz = 0.0f;
    m_whiteStreak = 0;
    m_lastGoodColumn.clear();
    if (!m_waterfallImage.isNull()) {
        m_waterfallImage.fill(Qt::black);
    }
    update();
}

void SpectrumWidget::onSpectrumData(QVector<float> powerSpectrum, float centerFreq, float binWidth)
{
    updateSpectrum(powerSpectrum, centerFreq, binWidth);
}

void SpectrumWidget::paintEvent(QPaintEvent * /*event*/)
{
    QElapsedTimer paintTimer;
    paintTimer.start();

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    const int plotWidth = std::max(1, width() - 60);
    const int plotHeight = std::max(1, height() - 60);
    const int plotX = 40;
    const int plotY = 20;

    if (m_waterfallImage.isNull() || m_waterfallColumns.isEmpty()) {
        painter.setPen(Qt::white);
        painter.setFont(QFont("Courier", 9));
        painter.drawText(plotX + 10, plotY + 30, "Waiting for spectrum data...");
        return;
    }

    painter.drawImage(QRect(plotX, plotY, plotWidth, plotHeight), m_waterfallImage);

    const int numBins = m_waterfallColumns.last().size();
    if (numBins == 0) {
        return;
    }

    {
        painter.setPen(Qt::NoPen);
        const long now = QDateTime::currentMSecsSinceEpoch();
        for (const SignalData &signal : m_signals) {
            const long age = now - signal.timestamp;
            if (age > m_maxSignalAge) {
                continue;
            }
            const int binIndex = offsetToBinIndex(signal.freqOffsetHz, numBins);
            const int y = binIndexToY(binIndex, numBins, plotY, plotHeight);
            const int markerX = plotX + plotWidth - 8;
            int alpha = 255 - static_cast<int>((age / static_cast<float>(m_maxSignalAge)) * 220);
            alpha = std::max(40, std::min(255, alpha));
            const QColor g(255, 255, 0, alpha);
            painter.setBrush(g);
            painter.setPen(QPen(QColor(200, 180, 0, alpha), 1));
            const int sz = 5;
            painter.drawRect(markerX - sz / 2, y - sz / 2, sz, sz);
            painter.setPen(QPen(Qt::white, 1));
            painter.drawPoint(markerX, y);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(Qt::white);
    }

    /* Selected tone for oscilloscope trace */
    if (m_hasTraceSelection) {
        const int binIndex = offsetToBinIndex(m_traceOffsetHz, numBins);
        const int y = binIndexToY(binIndex, numBins, plotY, plotHeight);
        painter.setPen(QPen(QColor(0, 255, 120), 1, Qt::DashLine));
        painter.drawLine(plotX, y, plotX + plotWidth, y);
        painter.setBrush(QColor(0, 255, 120));
        painter.setPen(QPen(QColor(0, 180, 80), 1));
        painter.drawEllipse(QPoint(plotX + plotWidth - 10, y), 5, 5);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QColor(0, 255, 120));
        painter.setFont(QFont("Courier", 8));
        painter.drawText(plotX + 6, y - 4,
                         QString("TRACE %1 Hz").arg(m_traceOffsetHz, 0, 'f', 0));
    }

    painter.setPen(Qt::white);
    painter.setFont(QFont("Courier", 8));
    painter.drawText(plotX - 35, plotY - 5, "Freq");

    if (m_binWidth > 0 && numBins > 0) {
        /* Narrow spans need more RF digits so labels still change when tuning */
        const float spanHz = m_binWidth * static_cast<float>(numBins);
        const int mhzDigits = (spanHz < 10000.0f) ? 4 : 3;
        for (int i = 0; i <= 4; ++i) {
            const int y = plotY + (i * plotHeight) / 4;
            const int binIndex = (4 - i) * numBins / 4;
            const float offset_hz = (binIndex - numBins / 2.0f) * m_binWidth;
            const float freq_mhz = (m_centerFrequency + offset_hz) / 1000000.0f;
            painter.drawText(plotX - 62, y + 4,
                             QString::number(freq_mhz, 'f', mhzDigits));
        }
    }

    painter.drawText(plotX + plotWidth - 50, plotY + plotHeight + 20, "Time");
    painter.drawText(plotX + 5, plotY + plotHeight + 20, "Old");
    painter.drawText(plotX + plotWidth - 30, plotY + plotHeight + 20, "New");

    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(plotX, plotY + plotHeight, plotX + plotWidth, plotY + plotHeight);
    painter.drawLine(plotX, plotY, plotX, plotY + plotHeight);

    const double paintMs = paintTimer.nsecsElapsed() / 1e6;
    m_perfPaintCount++;
    m_perfPaintTotalMs += paintMs;
    if (paintMs > m_perfPaintMaxMs) {
        m_perfPaintMaxMs = paintMs;
    }
}

void SpectrumWidget::resizeEvent(QResizeEvent *event)
{
    const int newScrollWidth = std::max(200, width() - 60);
    const int plotWidth = std::max(1, width() - 60);
    const int plotHeight = std::max(1, height() - 60);

    m_scrollWidth = newScrollWidth;
    while (m_waterfallColumns.size() > m_scrollWidth) {
        m_waterfallColumns.removeFirst();
    }

    if (plotWidth != m_plotWidth || plotHeight != m_plotHeight) {
        m_plotWidth = plotWidth;
        m_plotHeight = plotHeight;
        m_waterfallImage = QImage(plotWidth, plotHeight, QImage::Format_RGB32);
        rebuildWaterfallImage();
    }

    QWidget::resizeEvent(event);
}