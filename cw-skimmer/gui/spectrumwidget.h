/**
 * @file spectrumwidget.h
 * @brief Spectrum and waterfall display
 */

#ifndef SPECTRUMWIDGET_H
#define SPECTRUMWIDGET_H

#include <QWidget>
#include <QRect>
#include <QVector>
#include <QColor>
#include <QImage>
#include <QMouseEvent>
#include <QString>

class SpectrumWidget : public QWidget {
    Q_OBJECT

public:
    SpectrumWidget(QWidget *parent = nullptr);

    void addSignal(float frequency, float freqOffsetHz, float snr);
    void updateSpectrum(const QVector<float> &powerSpectrum, float centerFreq, float binWidth);
    void appendSpectrumColumns(const QVector<QVector<float>> &columns, float centerFreq, float binWidth);
    float snapOffsetHz(float freqOffsetHz) const;
    void clear();

    float centerFrequency() const { return m_centerFrequency; }
    float binWidth() const { return m_binWidth; }
    bool hasTraceSelection() const { return m_hasTraceSelection; }
    float traceOffsetHz() const { return m_traceOffsetHz; }
    void clearTraceSelection();
    void setTraceOffsetHz(float offsetHz);
    int consecutiveWhiteColumns() const { return m_whiteStreak; }

    /**
     * Power (dB) at IF offset for one spectrum column.
     * Averages the max over centerBin ± halfBins.
     */
    static float powerAtOffset(const QVector<float> &spectrum, float freqOffsetHz,
                               float binWidth, int halfBins);

    /**
     * Find strongest bin near centerOffsetHz within ±searchHalfHz.
     * Uses parabolic interpolation for a sub-bin peak estimate.
     * @return peak IF offset Hz; optional powerDbOut / snrDbOut
     */
    static float peakOffsetNear(const QVector<float> &spectrum, float centerOffsetHz,
                                float binWidth, float searchHalfHz,
                                float *powerDbOut = nullptr,
                                float noiseFloorDb = -200.0f,
                                float *snrDbOut = nullptr);

    /** Historical power series from current waterfall (oldest → newest). */
    QVector<float> powerHistoryAtOffset(float freqOffsetHz, int halfBins) const;

    /** Noise-floor estimate of the newest spectrum column. */
    float latestNoiseFloorDb() const;

signals:
    void captureMarkRequested(float freqOffsetHz, float absFreqHz);
    /** Plain click on waterfall: open/retune single-signal oscilloscope. */
    void signalTraceRequested(float freqOffsetHz, float absFreqHz);
    /**
     * Emitted when a spectrum column looks "all white" (saturated) — often
     * precedes a UI freeze. Message includes min/max/noise/NaN stats.
     */
    void waterfallAnomaly(const QString &message);

public slots:
    void onSpectrumData(QVector<float> powerSpectrum, float centerFreq, float binWidth);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QRect plotRect() const;
    float yToFreqOffsetHz(int y) const;
    struct SignalData {
        float frequency;
        float freqOffsetHz;
        float snr;
        long timestamp;
    };

    struct SpectrumStats {
        int numBins = 0;
        int nanCount = 0;
        int infCount = 0;
        int finiteCount = 0;
        float minDb = 0.0f;
        float maxDb = 0.0f;
        float meanDb = 0.0f;
        float medianDb = 0.0f;
        float noiseDb = 0.0f;
        float hotFrac = 0.0f;   /* fraction above white-ish color map */
        float dynamicRangeDb = 0.0f;
        bool allWhite = false;
        bool allBlack = false;
        bool badData = false;
    };

    int offsetToBinIndex(float freqOffsetHz, int numBins) const;
    int binIndexToY(int binIndex, int numBins, int plotY, int plotHeight) const;

    QColor powerToColor(float power_db, float noise_floor_db) const;
    float estimateNoiseFloor(const QVector<float> &spectrum) const;
    SpectrumStats analyzeSpectrum(const QVector<float> &spectrum, float noiseFloorDb) const;
    void ensureWaterfallImage(int plotWidth, int plotHeight);
    void rebuildWaterfallImage();
    void scrollWaterfallLeft();
    void drawWaterfallColumn(int x, const QVector<float> &spectrum, float noise_floor_db);
    void appendWaterfallColumn(const QVector<float> &powerSpectrum);
    void handleWhiteWaterfall(const SpectrumStats &st, const QVector<float> &spectrum);

    QVector<QVector<float>> m_waterfallColumns;  // One FFT column per pixel column; newest at end
    QImage m_waterfallImage;
    int m_plotWidth;
    int m_plotHeight;
    float m_noiseFloorDb;
    float m_centerFrequency;
    float m_binWidth;
    QVector<SignalData> m_signals;
    int m_maxSignalAge;  // in ms
    int m_scrollWidth;   // Pixel columns visible (updated on resize)

    bool m_hasTraceSelection;
    float m_traceOffsetHz;

    /* All-white / freeze diagnostics */
    int m_whiteStreak;
    int m_whiteEventCount;
    qint64 m_lastWhiteLogMs;
    float m_lastGoodNoiseDb;
    QVector<float> m_lastGoodColumn;

    qint64 m_perfUpdateCount;
    double m_perfUpdateTotalMs;
    double m_perfUpdateMaxMs;
    double m_perfScrollTotalMs;
    double m_perfDrawColTotalMs;
    double m_perfPaintTotalMs;
    double m_perfPaintMaxMs;
    int m_perfPaintCount;
};

#endif // SPECTRUMWIDGET_H


