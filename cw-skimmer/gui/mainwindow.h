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
class AudioMonitor;
class DecodeWidget;
class SettingsDialog;
class SignalTraceWindow;
class ThresholdMorseWindow;
class MaskMorseWindow;
class SpectrumMorseWindow;
class CwskMorseWindow;
class DeepcwMorseWindow;
class MultiChannelDecoder;
class CwKeyboard;
class QLineEdit;
class QPushButton;
class QScrollBar;
class QStackedWidget;

enum class MorseDecoderBackend {
    Threshold = 0,
    Mask = 1,
    Spectrum = 2,
    Cwsk = 3,
    DeepCw = 4
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
                        int samplesProcessed, float cpuUsage, int queueSize,
                        bool audioOnly);
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
    void onBasebandReady(QByteArray iq, int sampleRate, bool realAudio);
    void onToggleMorseBackend();
    void onToggleTciStreamMode();
    void onRecordingStoppedForSave();
    void onRecordToggled(bool checked);
    void onRecordingActiveChanged(bool active);
    void onChooseTrainingFile();
    void onStartPlaybackClicked();
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
    void updateConnectionIndicator();
    void stopDetection(const QString &reason);
    void ensureTraceWindow();
    void ensureMorsePanels();
    void showActiveMorsePanel();
    void updateMorseBackendButton();
    void updateTciStreamButton();
    void syncBasebandTap();
    double cwskPitchHz(bool realAudio) const;
    void applyTciStreamMode();
    void setTrainingFile(const QString &path);
    QString trainingBrowseDirectory() const;
    void updateMonitorChannel();
    void reseedTraceFromWaterfall();
    void feedTraceFromColumns(const QVector<QVector<float>> &columns, float binWidth);
    void syncSpectrumScroll(int numBins, float binWidth);
    void applySpectrumView();
    int spectrumViewSpanHz(int fullSpanHz) const;
    void syncTuneSlider(float offsetHz);
    void updateTuneReadout(float offsetHz);
    void updateTuneSliderRange(int numBins, float binWidth);
    void commitTuneSlider();
    void revealTuneOffset(float offsetHz);
    void centerSpectrumOn(float displayHz);
    int spectrumFullLowHz(int fullSpanHz) const;
    float traceDisplayHz() const;
    void applyTraceMarker(float fftHz);
    bool tuneSliderHeld() const;
    double radioTuneHz(float pictureOffsetHz) const;
    void syncSpectrumRfSign();
    void requestRadioTune(float pictureOffsetHz);
    void followRadioTune(float newCenterHz, const QVector<float> &column, float binWidth);
    /* Reported VFO after a click. The IQ picture stays on the pre-tune center. */
    float pictureCenterForColumns(float reportedCenter);
    void refreshMorseLabels();
    void syncCwKeyboard();
    QString cwTransportLabel() const;
    void sendFunctionKey(int index);
    void loadFunctionKeys();
    void saveFunctionKeys() const;

    // UI Widgets
    SpectrumWidget *m_spectrumWidget;
    DecodeWidget *m_decodeWidget;
    QScrollBar *m_spectrumScroll;
    class QSlider *m_tuneSlider;
    QLabel *m_tuneReadout;
    int m_spectrumBins;
    float m_spectrumBinWidth;
    float m_spectrumViewLow;
    bool m_spectrumViewSet;
    SignalTraceWindow *m_traceWindow;
    QStackedWidget *m_morseStack;
    ThresholdMorseWindow *m_morseWindow;
    MaskMorseWindow *m_maskWindow;
    SpectrumMorseWindow *m_scopeWindow;
    CwskMorseWindow *m_cwskWindow;
    DeepcwMorseWindow *m_deepcwWindow;
    MultiChannelDecoder *m_multiDecoder;
    QPushButton *m_morseBackendButton;
    QPushButton *m_tciStreamButton;
    QPushButton *m_recordButton;
    QPushButton *m_trainingFileButton;
    QPushButton *m_playTrainingButton;
    QString m_trainingFile;
    bool m_trainingPlayback;
    AudioMonitor *m_audioMonitor;
    MorseDecoderBackend m_morseBackend;
    bool m_cwskRetune;
    bool m_deepcwRetune;
    TciStreamMode m_tciStreamMode;

    // Worker thread
    QThread *m_workerThread;
    DetectorWorker *m_worker;

    // Status bar widgets
    QLabel *m_statusLabel;
    QLabel *m_connectionLabel;
    QLabel *m_connectLamp;
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
    CwKeyboard *m_cwKeyboard;
    QLineEdit *m_fnKeyEdit[5];
    bool m_radioTunePending;
    bool m_radioTuneIq;
    bool m_iqConjugate;
    double m_radioTuneHz;
    double m_radioTuneFromCenter;
    float m_radioTunePicture;
    int m_radioTuneSettle;
    /* IQ bins stay put when the VFO moves, so labels keep the pre-tune center. */
    bool m_holdPictureCenter;
    double m_pictureAnchorHz;
};

#endif // MAINWINDOW_H
