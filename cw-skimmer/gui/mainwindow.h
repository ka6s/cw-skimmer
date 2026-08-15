/**
 * @file mainwindow.h
 * @brief Main application window
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QThread>
#include "detectorworker.h"

class SpectrumWidget;
class DecodeWidget;
class SettingsDialog;
class SignalTraceWindow;
class ThresholdMorseWindow;
class MaskMorseWindow;
class MultiChannelDecoder;
class QPushButton;
class QStackedWidget;

enum class MorseDecoderBackend {
    Threshold = 0,
    Mask = 1
};

enum class TciStreamMode {
    IQ = 0,
    Audio = 1
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStartClicked();
    void onStopClicked();
    void onSettingsClicked();
    void onClearClicked();
    void onSignalDetected(float frequency, float freqOffsetHz, float snr, float confidence,
                          float tonePurity, float bandwidth);
    void onSpotReported(QString callsign, float frequency, float snr, float confidence);
    void onStatsUpdated(int numSignals, float noiseFloor, float avgPeakSnr, float peakSnr,
                        int spectrumPeakCount, int bufferFill, bool connected,
                        int samplesProcessed, float cpuUsage, int queueSize);
    void onSpectrumColumnsReady(QVector<QVector<float>> columns, float centerFreq, float binWidth);
    void onDecodeUpdated(QString decodedText, float frequencyHz,
                         float freqOffsetHz, float confidence);
    void onLogMessage(QString message, int level);
    void onWorkerStatusChanged(bool running);
    void onWorkerError(QString error);
    void onAbout();
    void onCaptureMarkRequested(float freqOffsetHz, float absFreqHz);
    void onSignalTraceRequested(float freqOffsetHz, float absFreqHz);
    void onTraceWindowClosed();
    void onTraceTuningChanged();
    void onTraceThresholdChanged(float thresholdDb);
    void onTraceSampleReady(float powerDb, bool aboveThreshold, qint64 sampleTimeMs);
    void onToggleMorseBackend();
    void onToggleTciStreamMode();
    void onRecordingStoppedForSave();
    void onRecordToggled(bool checked);
    void onRecordingActiveChanged(bool active);
    void onPlayCapture();
    void onPlaybackStarted(const QString &path);
    void onPlaybackFinished();
    void onSpectrumModeWide();
    void onSpectrumModeNarrow3k();

private:
    void createUI();
    void createMenuBar();
    void createToolBar();
    void createStatusBar();
    void setupConnections();
    void initializeWorker();
    void updateStatusBar();
    void stopDetection(const QString &reason);
    void ensureTraceWindow();
    void ensureMorsePanels();
    void showActiveMorsePanel();
    void updateMorseBackendButton();
    void updateTciStreamButton();
    void applyTciStreamMode();
    void reseedTraceFromWaterfall();
    void feedTraceFromColumns(const QVector<QVector<float>> &columns, float binWidth);

    // UI Widgets
    SpectrumWidget *m_spectrumWidget;
    DecodeWidget *m_decodeWidget;
    SignalTraceWindow *m_traceWindow;
    QStackedWidget *m_morseStack;
    ThresholdMorseWindow *m_morseWindow;
    MaskMorseWindow *m_maskWindow;
    MultiChannelDecoder *m_multiDecoder;
    QPushButton *m_morseBackendButton;
    QPushButton *m_tciStreamButton;
    QPushButton *m_recordButton;
    MorseDecoderBackend m_morseBackend;
    TciStreamMode m_tciStreamMode;

    // Worker thread
    QThread *m_workerThread;
    DetectorWorker *m_worker;

    // Status bar widgets
    QLabel *m_statusLabel;
    QLabel *m_connectionLabel;
    QLabel *m_bufferLabel;
    QLabel *m_cpuLabel;
    QLabel *m_queueLabel;
    QLabel *m_snrLabel;

    // Current status
    bool m_isRunning;
    bool m_freezeDisplay;
    bool m_captureInProgress;
    bool m_isConnected;
    int m_bufferFill;
    float m_cpuUsage;
    int m_queueSize;
    float m_avgPeakSnr;
    float m_peakSnr;
    int m_spectrumPeakCount;
    float m_lastCenterFreqHz;
    QString m_spectrumModeLabel;  /* "WIDE" or "NARROW 3k" */
};

#endif // MAINWINDOW_H
