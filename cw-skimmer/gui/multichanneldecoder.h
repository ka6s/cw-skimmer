/**
 * @file multichanneldecoder.h
 * @brief One Spectrum decoder for each 1 kHz slice of the waterfall
 *
 * A 48 kHz spectrum is 48 slices. Each slice follows the strongest signal
 * inside it. A small drift stays on the same decoder. A louder signal
 * elsewhere in the slice takes over after it has been the strongest for
 * two columns, and the copy already decoded stays with it. That copy
 * also stays after the CW stops.
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
    /* 48 kHz / 1 kHz. A wider span still gets one slice per 1 kHz, up to this. */
    static const int kMaxChannels = 48;
    static constexpr int kSlotHz = 1000;
    /* Tail kept per channel. The side list shows a window into that buffer. */
    static const int kDisplayChars = 256;

    /** How many 1 kHz slices cover this spectrum span. */
    static int slotCountForSpan(float spanHz);
    /** Slice index that contains offsetHz. */
    static int slotForOffset(float offsetHz, float spanHz);

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
        int slot;
        float freqOffsetHz;
        float frequencyHz;
        float snrDb;
        QString text;
        bool active;
    };

    QVector<ChannelView> channels() const;

    /** Copy already decoded in the 1 kHz slice that contains offsetHz. */
    QString textForOffset(float offsetHz) const;

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
        bool sustained;  /* seen on a later column near the followed frequency */
        int bornSerial;
        float switchHz;
        int switchHits;
    };

    bool slotEnabled(int slot) const;
    bool findStrongestPeak(const QVector<float> &spectrum, float binWidth,
                           float noiseFloorDb, float lowHz, float highHz,
                           PeakCand &out) const;
    void trackSlot(Channel &ch, const QVector<float> &spectrum, float binWidth,
                   float noiseFloorDb, float slotLow, float slotHigh,
                   qint64 nowMs);
    void armSlot(Channel &ch, const PeakCand &pk, qint64 nowMs);
    void retargetSlot(Channel &ch, const PeakCand &pk, qint64 nowMs);
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
    int m_naturalSlots;
    float m_spanHz;

    /* Same-signal follow distance. A louder peak farther than this, held for
     * two columns, moves the slice. The copy already decoded is kept. With
     * nothing else in the slice, a quiet signal is left where it is. */
    static constexpr float kFollowHz = 150.f;
    static constexpr float kSwitchMarginDb = 1.5f;
    static constexpr int kSwitchColumns = 2;
    static const int kThreshHistMax = 160;
    /* Same rise as the spectrum decoder's white trace. A quieter bin is not
     * a signal that decoder can copy. */
    static const float kMinSnrDb;
};

#endif // MULTICHANNELDECODER_H
