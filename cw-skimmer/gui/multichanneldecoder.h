/**
 * @file multichanneldecoder.h
 * @brief Parallel CW decode on up to 16 strongest spectrum peaks
 *
 * Backends (setBackend):
 *  - Threshold: headless ThresholdMorseWindow per channel
 *  - Mask:      headless MaskMorseWindow — dit/dah trapezoid overlay
 */

#ifndef MULTICHANNELDECODER_H
#define MULTICHANNELDECODER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QElapsedTimer>

class ThresholdMorseWindow;
class MaskMorseWindow;

class MultiChannelDecoder : public QObject {
    Q_OBJECT

public:
    static const int kMaxChannels = 16;
    static const int kDisplayChars = 10;

    enum class Backend {
        Threshold = 0,
        Mask = 1
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
        bool active;
    };

    QVector<PeakCand> findTopPeaks(const QVector<float> &spectrum, float binWidth,
                                   float noiseFloorDb, int maxPeaks) const;
    int matchChannel(float offsetHz) const;
    int allocateChannel(float offsetHz, float snrDb);
    void dropStaleChannels(qint64 nowMs);
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
    qint64 m_lastEmitMs;

    static const int kMatchHz = 80;
    static const int kMinPeakSeparationHz = 120;
    static const int kChannelTimeoutMs = 12000;
    static const int kThreshHistMax = 160;
    static const float kMinSnrDb;
};

#endif // MULTICHANNELDECODER_H
