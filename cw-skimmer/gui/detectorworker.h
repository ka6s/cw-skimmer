/**
 * @file detectorworker.h
 * @brief Qt worker thread for CW detector
 *
 * Thread model:
 *  - DetectorWorker lives on a QThread (affinity).
 *  - start()/stop()/setConfig() must run on that thread (Queued/BlockingQueued).
 *  - Detection runs in a dedicated pthread that only touches the C API and
 *    posts results via signals (queued to the GUI thread).
 */

#ifndef DETECTORWORKER_H
#define DETECTORWORKER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QMetaType>
#include <QElapsedTimer>
#include <QMutex>
#include <pthread.h>
#include <atomic>
#include "../src/cwskimmer_api.h"

Q_DECLARE_METATYPE(QVector<float>)
Q_DECLARE_METATYPE(QVector<QVector<float>>)

class DetectorWorker : public QObject {
    Q_OBJECT

public:
    DetectorWorker(const QString &configFile = "");
    ~DetectorWorker() override;

    bool initialize();
    void shutdown();

    /** Safe from any thread (atomic). */
    bool isRunning() const;
    bool isDetectionActive() const;

    void pauseGuiUpdates();
    void resumeGuiUpdates();
    bool freezeCapture();
    void clearFrozenCapture();
    bool saveCapture(const QString &path, float markFreqOffsetHz,
                     const QString &expectedText, const QString &notes);

public slots:
    /** Must be invoked on the worker thread. */
    void start();
    /** Must be invoked on the worker thread (joins detection pthread). */
    void stop();
    void setConfig(const QString &key, const QString &value);

signals:
    void signalDetected(float frequency, float freqOffsetHz, float snr, float confidence,
                        float tonePurity, float bandwidth);
    void spotReported(QString callsign, float frequency, float snr, float confidence);
    void statsUpdated(int numSignals, float noiseFloor, float avgPeakSnr, float peakSnr,
                      int spectrumPeakCount, int bufferFill, bool connected,
                      int samplesProcessed, float cpuUsage, int queueSize);
    void spectrumColumnsReady(QVector<QVector<float>> columns, float centerFreq, float binWidth);
    void decodeUpdated(QString decodedText, float frequencyHz,
                       float freqOffsetHz, float confidence);
    void logMessage(QString message, int level);
    void errorOccurred(QString error);
    void statusChanged(bool running);

private:
    static void *detectionThreadEntry(void *arg);
    static void signalCallbackStatic(const cwskimmer_signal_t *signal, void *userdata);
    static void spotCallbackStatic(const cwskimmer_spot_t *spot, void *userdata);
    static void statsCallbackStatic(const cwskimmer_stats_t *stats, void *userdata);
    static void spectrumCallbackStatic(const cwskimmer_spectrum_t *spectrum, void *userdata);
    static void decodeCallbackStatic(const cwskimmer_decode_t *decode, void *userdata);
    static void logCallbackStatic(const char *message, int level, void *userdata);

    void runDetectionLoop();
    void onSignalDetected(const cwskimmer_signal_t *signal);
    void onSpotReported(const cwskimmer_spot_t *spot);
    void onStatsUpdated(const cwskimmer_stats_t *stats);
    void onSpectrumUpdated(const cwskimmer_spectrum_t *spectrum);
    void onDecodeUpdated(const cwskimmer_decode_t *decode);
    void onLogMessage(const char *message, int level);
    void flushPendingSpectrum();
    void joinDetectionThread();
    bool waitForLoopInactive(int timeoutMs);

    cwskimmer_detector_t *m_detector;
    QString m_configFile;

    QMutex m_spectrumMutex;
    QVector<QVector<float>> m_pendingSpectrumColumns;
    float m_pendingCenterFreq;
    float m_pendingBinWidth;
    QElapsedTimer m_spectrumBatchTimer;
    bool m_spectrumBatchTimerActive;

    pthread_t m_detectThread;
    bool m_detectThreadLaunched;
    std::atomic<bool> m_detectThreadActive;
    std::atomic<bool> m_acceptCallbacks;
};

#endif // DETECTORWORKER_H
