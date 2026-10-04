/**
 * @file multichanneldecoder.h
 * @brief Parallel CW decode on up to 16 strongest spectrum peaks
 *
 * Each active channel owns one Spectrum decoder locked to the bin where
 * that signal was acquired. The copy stays after the CW stops. A channel
 * is cleared only when a new signal takes its place: an empty slot is used
 * first, then a signal that never stayed on, then the weakest held signal
 * when the newcomer is stronger.
 *
 * Backends (setBackend):
 *  - Threshold: headless ThresholdMorseWindow per channel
 *  - Mask:      headless MaskMorseWindow — dit/dah trapezoid overlay
 *  - Spectrum:  white/black waterfall runs, copied 6 dahs behind
 */

#ifndef MULTICHANNELDECODER_H
#define MULTICHANNELDECODER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QElapsedTimer>

class ThresholdMorseWindow;
class MaskMorseWindow;
class SpectrumMorseWindow;

class MultiChannelDecoder : public QObject {
    Q_OBJECT

public:
    static const int kMaxChannels = 16;
    /* Tail kept per channel. The side list shows only what fits, and drops
     * the leftmost character when the next one is copied. */
    static const int kDisplayChars = 256;

    enum class Backend {
        Threshold = 0,
        Mask = 1,
        Spectrum = 2
    };

    explicit MultiChannelDecoder(QObject *parent = nullptr);
    ~MultiChannelDecoder() override;

    void clear();
    void setCenterFrequency(float centerHz) { m_centerHz = centerHz; }

    bool setBackend(Backend backend, QString *errorOut = nullptr);
    Backend backend() const { return m_backend; }

    void setMaxActiveChannels(int n);
    int maxActiveChannels() const { return m_maxActive; }

    void setHalfBins(int halfBins);
    int halfBins() const { return m_halfBins; }

    void processSpectrumColumn(const QVector<float> &spectrum, float binWidth,
                               float centerHz, float noiseFloorDb);

    struct ChannelView {
        float freqOffsetHz;
        float frequencyHz;
        float snrDb;
        QString text;
        bool active;
    };

    QVector<ChannelView> channels() const;

signals:
    void channelsUpdated(const QVector<MultiChannelDecoder::ChannelView> &channels);

private:
    struct PeakCand {
        float offsetHz;
        float powerDb;
        float snrDb;
    };

    struct Channel {
        float offsetHz;
        float frequencyHz;
        float snrDb;
        float thrDb;
        float trackLowDb;
        float trackHighDb;
        float noisePeakDb;
        float markPeakHoldDb;
        float markBodyDb;
        float noiseEmaDb;
        bool trackInit;
        bool keyHigh;
        float lastPowerDb;
        qint64 lastSeenMs;
        QVector<float> powerHist;
        ThresholdMorseWindow *thrDecoder;
        MaskMorseWindow *maskDecoder;
        SpectrumMorseWindow *scopeDecoder;
        bool active;
        bool sustained;  /* seen on a later column, so a one-bin spike cannot hold the slot */
        int bornSerial;
    };

    struct PendingPeak {
        float offsetHz;
        float snrDb;
        int hits;
        int lastSerial;
    };

    QVector<PeakCand> findTopPeaks(const QVector<float> &spectrum, float binWidth,
                                   float noiseFloorDb, int maxPeaks) const;
    int matchChannel(float offsetHz) const;
    int allocateChannel(float offsetHz, float snrDb, bool replaceSustained);
    void rememberPending(const PeakCand &pk);
    void updateChannelThreshold(Channel &ch, float powerDb, float noiseFloorDb);
    void emitSnapshot();
    void resetChannelState(Channel &ch);
    void destroyChannelEngines(Channel &ch);
    void feedChannel(Channel &ch, float powerDb, float noiseFloorDb, qint64 nowMs);
    QString channelText(const Channel &ch) const;

    Channel m_channels[kMaxChannels];
    float m_centerHz;
    int m_halfBins;
    int m_maxActive;
    Backend m_backend;
    QElapsedTimer m_clock;
    qint64 m_columnClockMs; /* one FFT hop per column, not one stamp per GUI batch */
    qint64 m_lastEmitMs;
    int m_columnSerial;
    QVector<PendingPeak> m_pending;

    /* One decoder per 2 kHz window. A peak inside that window stays on the
     * same decoder; the next signal has to sit a full 2 kHz away. */
    static const int kMatchHz = 2000;
    static const int kMinPeakSeparationHz = 2000;
    static const int kThreshHistMax = 160;
    /* Same rise as the spectrum decoder's white trace. A quieter bin is not
     * a signal that decoder can copy. */
    static const float kMinSnrDb;
};

#endif // MULTICHANNELDECODER_H
