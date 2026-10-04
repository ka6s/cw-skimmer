/**
 * @file mainwindow.cpp
 * @brief Main window implementation
 */

#include "mainwindow.h"
#include "spectrumwidget.h"
#include "decodewidget.h"
#include "settingsdialog.h"
#include "signaltracewindow.h"
#include "thresholdmorsewindow.h"
#include "maskmorsewindow.h"
#include "spectrummorsewindow.h"
#include "multichanneldecoder.h"
#include "audiomonitor.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QToolBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QScrollBar>
#include <QStatusBar>
#include <QMessageBox>
#include <QThread>
#include <QApplication>
#include <QCoreApplication>
#include <QInputDialog>
#include <QDialog>
#include <QEventLoop>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QDateTime>
#include <QStackedWidget>
#include <QGroupBox>
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace {

QString captureDirectory()
{
    QDir base(QCoreApplication::applicationDirPath());
    if (base.dirName() == "bin") {
        base.cdUp();
    }
    const QString dirPath = base.filePath("captures");
    QDir().mkpath(dirPath);
    return dirPath;
}

}  // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_spectrumWidget(nullptr)
    , m_decodeWidget(nullptr)
    , m_spectrumScroll(nullptr)
    , m_spectrumBins(0)
    , m_spectrumBinWidth(0.0f)
    , m_spectrumViewLow(0.0f)
    , m_spectrumViewSet(false)
    , m_traceWindow(nullptr)
    , m_morseStack(nullptr)
    , m_morseWindow(nullptr)
    , m_maskWindow(nullptr)
    , m_scopeWindow(nullptr)
    , m_multiDecoder(nullptr)
    , m_morseBackendButton(nullptr)
    , m_tciStreamButton(nullptr)
    , m_recordButton(nullptr)
    , m_trainingFileButton(nullptr)
    , m_playTrainingButton(nullptr)
    , m_trainingPlayback(false)
    , m_audioMonitor(nullptr)
    , m_morseBackend(MorseDecoderBackend::Spectrum)
    , m_tciStreamMode(TciStreamMode::IQ)
    , m_workerThread(nullptr)
    , m_worker(nullptr)
    , m_statusLabel(nullptr)
    , m_connectionLabel(nullptr)
    , m_connectLamp(nullptr)
    , m_bufferLabel(nullptr)
    , m_cpuLabel(nullptr)
    , m_queueLabel(nullptr)
    , m_snrLabel(nullptr)
    , m_isRunning(false)
    , m_freezeDisplay(false)
    , m_captureInProgress(false)
    , m_isConnected(false)
    , m_bufferFill(0)
    , m_cpuUsage(0.0f)
    , m_queueSize(0)
    , m_avgPeakSnr(0.0f)
    , m_peakSnr(0.0f)
    , m_spectrumPeakCount(0)
    , m_lastCenterFreqHz(0.0f)
    , m_spectrumModeLabel(QStringLiteral("WIDE"))
{
    setWindowTitle("CW Skimmer");
    // Icon intentionally omitted (no embedded resources in current build)
    setGeometry(100, 100, 1200, 800);

    createUI();
    createMenuBar();
    createToolBar();
    createStatusBar();
    setupConnections();
    initializeWorker();
}

MainWindow::~MainWindow()
{
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
    }
}

void MainWindow::createUI()
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    m_spectrumWidget = new SpectrumWidget();
    m_decodeWidget = new DecodeWidget();
    m_multiDecoder = new MultiChannelDecoder(this);
    m_multiDecoder->setMaxActiveChannels(MultiChannelDecoder::kMaxChannels);
    m_multiDecoder->setBackend(MultiChannelDecoder::Backend::Spectrum);
    connect(m_multiDecoder, &MultiChannelDecoder::channelsUpdated, this,
            [this](const QVector<MultiChannelDecoder::ChannelView> &channels) {
                if (!m_decodeWidget) {
                    return;
                }
                m_decodeWidget->setChannels(channels);
            });

    /* Record sits above the spectrum (scope still owns the capture buffer). */
    m_recordButton = new QPushButton(QStringLiteral("Record"), centralWidget);
    m_recordButton->setCheckable(true);
    m_recordButton->setChecked(false);
    m_recordButton->setToolTip(
        QStringLiteral(
            "ON: start capturing the Signal Trace envelope for the selected tone.\n"
            "OFF: stop and save .cwtrace + .wav under captures/.\n"
            "Click a signal on the waterfall first so the scope has a target."));
    connect(m_recordButton, &QPushButton::toggled, this, &MainWindow::onRecordToggled);

    auto *spectrumBar = new QHBoxLayout();
    spectrumBar->setContentsMargins(0, 0, 0, 2);
    spectrumBar->addWidget(m_recordButton, 0, Qt::AlignLeft);
    spectrumBar->addStretch(1);

    /* Decode lines share the waterfall's height so each copy sits on its trace. */
    auto *spectrumPane = new QWidget(centralWidget);
    auto *paneLayout = new QHBoxLayout(spectrumPane);
    paneLayout->setContentsMargins(0, 0, 0, 0);
    paneLayout->setSpacing(0);
    paneLayout->addWidget(m_spectrumWidget, 1);
    paneLayout->addWidget(m_decodeWidget, 0);
    m_spectrumScroll = new QScrollBar(Qt::Vertical, spectrumPane);
    m_spectrumScroll->setObjectName(QStringLiteral("spectrumScroll"));
    m_spectrumScroll->setRange(0, 0);
    m_spectrumScroll->setEnabled(false);
    m_spectrumScroll->setToolTip(
        QStringLiteral("Scrolls the 10 kHz spectrum window and the copy beside it together."));
    paneLayout->addWidget(m_spectrumScroll, 0);
    connect(m_spectrumScroll, &QScrollBar::valueChanged, this, [this](int) {
        applySpectrumView();
    });
    connect(m_spectrumWidget, &SpectrumWidget::verticalScrollRequested, this,
            [this](int deltaY) {
                if (!m_spectrumScroll || !m_spectrumScroll->isEnabled()) {
                    return;
                }
                const int step = std::max(1, m_spectrumScroll->singleStep());
                const int notches = (deltaY > 0) ? std::max(1, deltaY / 120)
                                                 : std::min(-1, deltaY / 120);
                m_spectrumScroll->setValue(m_spectrumScroll->value() - notches * step);
            });
    connect(m_decodeWidget, &DecodeWidget::verticalScrollRequested, this,
            [this](int deltaY) {
                if (!m_spectrumScroll || !m_spectrumScroll->isEnabled()) {
                    return;
                }
                const int step = std::max(1, m_spectrumScroll->singleStep());
                const int notches = (deltaY > 0) ? std::max(1, deltaY / 120)
                                                 : std::min(-1, deltaY / 120);
                m_spectrumScroll->setValue(m_spectrumScroll->value() - notches * step);
            });

    auto *spectrumColumn = new QWidget(centralWidget);
    auto *spectrumColumnLayout = new QVBoxLayout(spectrumColumn);
    spectrumColumnLayout->setContentsMargins(0, 0, 0, 0);
    spectrumColumnLayout->setSpacing(2);
    spectrumColumnLayout->addLayout(spectrumBar);
    spectrumColumnLayout->addWidget(spectrumPane, 1);

    QWidget *spectrumRow = new QWidget(centralWidget);
    QHBoxLayout *spectrumLayout = new QHBoxLayout(spectrumRow);
    spectrumLayout->setContentsMargins(0, 0, 0, 0);
    spectrumLayout->addWidget(spectrumColumn, 1);
    spectrumRow->setLayout(spectrumLayout);

    /* Bottom pane: embedded Morse decoder (Threshold / Mask stack) */
    m_morseStack = new QStackedWidget(centralWidget);
    m_morseWindow = new ThresholdMorseWindow(m_morseStack);
    m_maskWindow = new MaskMorseWindow(m_morseStack);
    m_scopeWindow = new SpectrumMorseWindow(m_morseStack);
    m_morseStack->addWidget(m_morseWindow);
    m_morseStack->addWidget(m_maskWindow);
    m_morseStack->addWidget(m_scopeWindow);
    m_morseStack->setCurrentWidget(m_scopeWindow);

    /* Dit/dah boxes live on the Signal Trace scope, not a second panel. */
    connect(m_maskWindow, &MaskMorseWindow::maskOverlayChanged, this,
            [this](double ditSec, double dahSec, float noiseFloorDb, float peakDb,
                   float ditScore, float dahScore,
                   bool looking, bool ditHit, bool dahHit) {
                if (!m_traceWindow || m_morseBackend != MorseDecoderBackend::Mask) {
                    return;
                }
                m_traceWindow->setMaskOverlay(true, ditSec, dahSec, noiseFloorDb, peakDb,
                                              ditScore, dahScore,
                                              looking, ditHit, dahHit);
            });

    auto *morseBox = new QGroupBox(QStringLiteral("Morse Decoder"), centralWidget);
    auto *morseBoxLayout = new QVBoxLayout(morseBox);
    morseBoxLayout->setContentsMargins(4, 8, 4, 4);
    morseBoxLayout->addWidget(m_morseStack);

    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->addWidget(spectrumRow, 3);
    mainLayout->addWidget(morseBox, 2);
    centralWidget->setLayout(mainLayout);
}

void MainWindow::createMenuBar()
{
    QMenuBar *menuBar = new QMenuBar(this);
    setMenuBar(menuBar);

    // File menu
    QMenu *fileMenu = menuBar->addMenu("&File");
    QAction *quitAction = fileMenu->addAction("&Quit");
    connect(quitAction, &QAction::triggered, this, &QApplication::quit);

    // Tools menu
    QMenu *toolsMenu = menuBar->addMenu("&Tools");
    QAction *settingsAction = toolsMenu->addAction("&Settings");
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsClicked);
    QAction *traceAction = toolsMenu->addAction("Signal &Trace…");
    connect(traceAction, &QAction::triggered, this, [this]() {
        ensureTraceWindow();
        if (m_traceWindow) {
            m_traceWindow->show();
            m_traceWindow->raise();
            m_traceWindow->activateWindow();
        }
    });
    QAction *trainingAction = toolsMenu->addAction("&Training file…");
    trainingAction->setToolTip("Choose a .tcistream file. Playback starts only from Start playback.");
    connect(trainingAction, &QAction::triggered, this, &MainWindow::onChooseTrainingFile);
    QAction *playbackAction = toolsMenu->addAction("Start &playback");
    playbackAction->setToolTip("Play the chosen training file through the detector");
    connect(playbackAction, &QAction::triggered, this, &MainWindow::onStartPlaybackClicked);
    toolsMenu->addSeparator();
    QAction *playAction = toolsMenu->addAction("&Play capture…");
    playAction->setToolTip(
        "Replay a saved .wav / .cwtrace through Signal Trace and Threshold Morse "
        "as if it were live RF");
    connect(playAction, &QAction::triggered, this, &MainWindow::onPlayCapture);

    toolsMenu->addSeparator();
    QAction *wideAction = toolsMenu->addAction("Spectrum: &Wide 48 kHz (working default)");
    wideAction->setToolTip(
        "Full 48 kHz waterfall, ~47 Hz bins, proven 2–5 WPM path. "
        "Use this to return to the current workable solution.");
    connect(wideAction, &QAction::triggered, this, &MainWindow::onSpectrumModeWide);

    QAction *narrowAction = toolsMenu->addAction("Spectrum: &Narrow 3 kHz (experiment)");
    narrowAction->setToolTip(
        "±1.5 kHz around VFO only, ~12 Hz bins, ~11 ms columns — try faster CW.\n"
        "Tune the radio so the signal is in the middle; waterfall RF labels follow VFO.");
    connect(narrowAction, &QAction::triggered, this, &MainWindow::onSpectrumModeNarrow3k);

    // Help menu
    QMenu *helpMenu = menuBar->addMenu("&Help");
    QAction *aboutAction = helpMenu->addAction("&About");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::createToolBar()
{
    QToolBar *toolBar = addToolBar("Main Toolbar");
    toolBar->setObjectName("MainToolBar");

    // Start button
    QPushButton *startButton = new QPushButton("Start");
    connect(startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    toolBar->addWidget(startButton);

    // Stop button
    QPushButton *stopButton = new QPushButton("Stop");
    connect(stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    toolBar->addWidget(stopButton);

    m_connectLamp = new QLabel(toolBar);
    m_connectLamp->setObjectName(QStringLiteral("connectLamp"));
    m_connectLamp->setAlignment(Qt::AlignCenter);
    m_connectLamp->setMinimumWidth(148);
    m_connectLamp->setToolTip(
        QStringLiteral("Green when Start has a live TCI session with the radio."));
    toolBar->addWidget(m_connectLamp);
    updateConnectionIndicator();

    toolBar->addSeparator();

    // Settings button
    QPushButton *settingsButton = new QPushButton("Settings");
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
    toolBar->addWidget(settingsButton);

    // Clear button
    QPushButton *clearButton = new QPushButton("Clear");
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::onClearClicked);
    toolBar->addWidget(clearButton);

    toolBar->addSeparator();
    m_trainingFileButton = new QPushButton("Training file…");
    m_trainingFileButton->setToolTip(
        "Choose a .tcistream training file. This does not start playback.");
    connect(m_trainingFileButton, &QPushButton::clicked, this, &MainWindow::onChooseTrainingFile);
    toolBar->addWidget(m_trainingFileButton);

    m_playTrainingButton = new QPushButton("Start playback");
    m_playTrainingButton->setToolTip("Play the chosen training file through the detector");
    m_playTrainingButton->setEnabled(false);
    connect(m_playTrainingButton, &QPushButton::clicked, this, &MainWindow::onStartPlaybackClicked);
    toolBar->addWidget(m_playTrainingButton);

    toolBar->addSeparator();
    QPushButton *playButton = new QPushButton("Play capture…");
    playButton->setToolTip("Replay .wav / .cwtrace through scope + Morse decoder");
    connect(playButton, &QPushButton::clicked, this, &MainWindow::onPlayCapture);
    toolBar->addWidget(playButton);

    toolBar->addSeparator();
    m_tciStreamButton = new QPushButton(this);
    m_tciStreamButton->setToolTip(
        "Toggle TCI radio stream type:\n"
        "  IQ — complex baseband (true ±Fs/2 waterfall; deskHPSDR iq_start)\n"
        "  Audio — demodulated stereo RX audio (mono spectrum; audio_start)\n"
        "Works before Start (preference) or live while connected.");
    connect(m_tciStreamButton, &QPushButton::clicked, this, &MainWindow::onToggleTciStreamMode);
    toolBar->addWidget(m_tciStreamButton);

    toolBar->addSeparator();
    m_morseBackendButton = new QPushButton(this);
    m_morseBackendButton->setToolTip(
        "Cycle Morse decoder:\n"
        "  Threshold — mark/space on the signal trace\n"
        "  Mask — dit/dah boxes on the signal trace\n"
        "  Spectrum — white/black runs on the waterfall, copied 6 dahs behind\n"
        "Beside the waterfall, each 2 kHz slice has its own Spectrum decoder\n"
        "and follows the strongest signal in that slice.\n"
        "Click one on the waterfall to copy its text into the Morse Decoder\n"
        "and keep decoding it there and beside the spectrum.");
    connect(m_morseBackendButton, &QPushButton::clicked, this, &MainWindow::onToggleMorseBackend);
    toolBar->addWidget(m_morseBackendButton);

    updateMorseBackendButton();
    updateTciStreamButton();

    toolBar->addSeparator();
    m_audioMonitor = new AudioMonitor(this);
    toolBar->addWidget(new QLabel(QStringLiteral("Audio out:"), toolBar));
    toolBar->addWidget(m_audioMonitor->deviceCombo());
    toolBar->addWidget(new QLabel(QStringLiteral("Vol"), toolBar));
    toolBar->addWidget(m_audioMonitor->volumeSlider());
}

void MainWindow::createStatusBar()
{
    m_statusLabel = new QLabel("Status: Idle");
    m_connectionLabel = new QLabel("Connected: NO");
    m_bufferLabel = new QLabel("Buffer: 0");
    m_cpuLabel = new QLabel("CPU: 0%");
    m_queueLabel = new QLabel("Queue: 0");
    m_snrLabel = new QLabel("Avg SNR: --");

    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_snrLabel, 0);
    statusBar()->addPermanentWidget(m_connectionLabel, 0);
    statusBar()->addPermanentWidget(m_bufferLabel, 0);
    statusBar()->addPermanentWidget(m_cpuLabel, 0);
    statusBar()->addPermanentWidget(m_queueLabel, 0);

    updateStatusBar();
}

void MainWindow::setupConnections()
{
    // These will be connected in initializeWorker after the worker thread is created
}

void MainWindow::initializeWorker()
{
    // Create worker thread
    m_workerThread = new QThread(this);
    m_worker = new DetectorWorker();
    m_worker->moveToThread(m_workerThread);

    // Initialize worker
    if (!m_worker->initialize()) {
        QMessageBox::critical(this, "Error", "Failed to initialize detector");
        return;
    }

    /* Prefer full 48 kHz wide waterfall as the default workable path */
    m_worker->setConfig(QStringLiteral("spectrum_span_hz"), QStringLiteral("0"));
    m_spectrumModeLabel = QStringLiteral("WIDE");
    applyTciStreamMode();

    if (m_multiDecoder) {
        bool channelsOk = false;
        const int configuredChannels =
            m_worker->configValue(QStringLiteral("multi_decode_channels")).toInt(&channelsOk);
        if (channelsOk && configuredChannels > 0) {
            m_multiDecoder->setMaxActiveChannels(configuredChannels);
        }
    }

    const QString configuredTraining = m_worker->configValue(QStringLiteral("training_file"));
    if (!configuredTraining.isEmpty()) {
        setTrainingFile(configuredTraining);
    }

    // Connect worker signals
    connect(m_worker, &DetectorWorker::signalDetected, this, &MainWindow::onSignalDetected);
    connect(m_worker, &DetectorWorker::spotReported, this, &MainWindow::onSpotReported);
    connect(m_worker, &DetectorWorker::statsUpdated, this, &MainWindow::onStatsUpdated);
    connect(m_worker, &DetectorWorker::spectrumColumnsReady, this, &MainWindow::onSpectrumColumnsReady,
            Qt::QueuedConnection);
    connect(m_worker, &DetectorWorker::decodeUpdated, this, &MainWindow::onDecodeUpdated,
            Qt::QueuedConnection);

    connect(m_worker, &DetectorWorker::logMessage, this, &MainWindow::onLogMessage);
    connect(m_worker, &DetectorWorker::statusChanged, this, &MainWindow::onWorkerStatusChanged);
    connect(m_worker, &DetectorWorker::errorOccurred, this, &MainWindow::onWorkerError);
    connect(m_spectrumWidget, &SpectrumWidget::captureMarkRequested,
            this, &MainWindow::onCaptureMarkRequested);
    connect(m_spectrumWidget, &SpectrumWidget::signalTraceRequested,
            this, &MainWindow::onSignalTraceRequested);
    connect(m_spectrumWidget, &SpectrumWidget::waterfallAnomaly, this,
            [this](const QString &msg) {
                /* Level 2 = warning — visible in log when waterfall goes white/freezes */
                onLogMessage(msg, 2);
            });

    // Start worker thread (detector starts only when user clicks Start)
    m_workerThread->start();
}

void MainWindow::ensureTraceWindow()
{
    if (m_traceWindow) {
        return;
    }
    m_traceWindow = new SignalTraceWindow(this);
    connect(m_traceWindow, &SignalTraceWindow::closed,
            this, &MainWindow::onTraceWindowClosed);
    connect(m_traceWindow, &SignalTraceWindow::tuningChanged,
            this, &MainWindow::onTraceTuningChanged);
    connect(m_traceWindow, &SignalTraceWindow::thresholdChanged,
            this, &MainWindow::onTraceThresholdChanged);
    connect(m_traceWindow, &SignalTraceWindow::sampleReady,
            this, &MainWindow::onTraceSampleReady);
    connect(m_traceWindow, &SignalTraceWindow::openMorseRequested,
            this, &MainWindow::showActiveMorsePanel);
    connect(m_traceWindow, &SignalTraceWindow::recordingStoppedForSave,
            this, &MainWindow::onRecordingStoppedForSave);
    connect(m_traceWindow, &SignalTraceWindow::recordingActiveChanged,
            this, &MainWindow::onRecordingActiveChanged);
    connect(m_traceWindow, &SignalTraceWindow::playbackStarted,
            this, &MainWindow::onPlaybackStarted);
    connect(m_traceWindow, &SignalTraceWindow::playbackFinished,
            this, &MainWindow::onPlaybackFinished);
    connect(m_traceWindow, &SignalTraceWindow::afcOffsetChanged,
            this, [this](float offsetHz) {
                if (m_spectrumWidget) {
                    m_spectrumWidget->setTraceOffsetHz(offsetHz);
                }
                updateMonitorChannel();
            });
    if (m_audioMonitor) {
        connect(m_traceWindow, &SignalTraceWindow::liveEnvelope,
                m_audioMonitor, &AudioMonitor::setTraceEnvelope);
    }
}

void MainWindow::ensureMorsePanels()
{
    /* Panels are created in createUI(); keep target labels current. */
    if (m_traceWindow && m_traceWindow->hasTarget()) {
        const QString label =
            QString("IF %1 Hz").arg(m_traceWindow->targetOffsetHz(), 0, 'f', 0);
        if (m_morseWindow) {
            m_morseWindow->setThresholdDb(m_traceWindow->thresholdDb());
            m_morseWindow->setTargetLabel(label);
        }
        if (m_maskWindow) {
            m_maskWindow->setTargetLabel(label);
        }
    }
}

void MainWindow::updateMorseBackendButton()
{
    if (!m_morseBackendButton) {
        return;
    }
    if (m_morseBackend == MorseDecoderBackend::Mask) {
        m_morseBackendButton->setText(QStringLiteral("Decoder: Mask"));
        m_morseBackendButton->setStyleSheet(
            "QPushButton { background: #4a1a6e; color: #e8ccff; font-weight: bold; "
            "padding: 4px 10px; }");
    } else if (m_morseBackend == MorseDecoderBackend::Spectrum) {
        m_morseBackendButton->setText(QStringLiteral("Decoder: Spectrum"));
        m_morseBackendButton->setStyleSheet(
            "QPushButton { background: #3a3a3a; color: #f4f4f4; font-weight: bold; "
            "padding: 4px 10px; }");
    } else {
        m_morseBackendButton->setText(QStringLiteral("Decoder: Threshold"));
        m_morseBackendButton->setStyleSheet(
            "QPushButton { background: #2a5a2a; color: #e0ffe0; font-weight: bold; "
            "padding: 4px 10px; }");
    }
}

void MainWindow::updateTciStreamButton()
{
    if (!m_tciStreamButton) {
        return;
    }
    if (m_tciStreamMode == TciStreamMode::Audio) {
        m_tciStreamButton->setText(QStringLiteral("Stream: Audio"));
        m_tciStreamButton->setStyleSheet(
            "QPushButton { background: #6e4a1a; color: #ffefcc; font-weight: bold; "
            "padding: 4px 10px; }");
    } else {
        m_tciStreamButton->setText(QStringLiteral("Stream: IQ"));
        m_tciStreamButton->setStyleSheet(
            "QPushButton { background: #1a3a6e; color: #cce0ff; font-weight: bold; "
            "padding: 4px 10px; }");
    }
}

void MainWindow::applyTciStreamMode()
{
    if (!m_worker) {
        return;
    }
    const QString mode = (m_tciStreamMode == TciStreamMode::Audio)
                             ? QStringLiteral("audio")
                             : QStringLiteral("iq");
    m_worker->setConfig(QStringLiteral("tci_stream_mode"), mode);
}

void MainWindow::onToggleTciStreamMode()
{
    if (m_tciStreamMode == TciStreamMode::IQ) {
        m_tciStreamMode = TciStreamMode::Audio;
        onLogMessage(
            "TCI stream → Audio (demodulated RX). Waterfall is mono/single-sided. "
            "Live switch if connected; else applies on next Start.",
            1);
    } else {
        m_tciStreamMode = TciStreamMode::IQ;
        onLogMessage(
            "TCI stream → IQ (complex baseband). Full ±Fs/2 waterfall when the radio "
            "supports iq_start. Live switch if connected; else applies on next Start.",
            1);
    }
    updateTciStreamButton();
    applyTciStreamMode();
    if (m_spectrumWidget) {
        m_spectrumWidget->clear();
    }
    updateStatusBar();
}

void MainWindow::showActiveMorsePanel()
{
    ensureMorsePanels();

    if (m_morseBackend == MorseDecoderBackend::Mask) {
        if (m_maskWindow && m_morseStack) {
            m_morseStack->setCurrentWidget(m_maskWindow);
        }
        return;
    }
    if (m_morseBackend == MorseDecoderBackend::Spectrum) {
        if (m_scopeWindow && m_morseStack) {
            m_morseStack->setCurrentWidget(m_scopeWindow);
        }
        return;
    }

    if (m_morseWindow && m_morseStack) {
        m_morseStack->setCurrentWidget(m_morseWindow);
    }
}

void MainWindow::reseedTraceFromWaterfall()
{
    if (!m_traceWindow || !m_traceWindow->hasTarget() || !m_spectrumWidget) {
        return;
    }
    const QVector<float> history = m_spectrumWidget->powerHistoryAtOffset(
        m_traceWindow->targetOffsetHz(), m_traceWindow->halfBins());
    m_traceWindow->seedHistory(history, m_spectrumWidget->latestNoiseFloorDb());
}

void MainWindow::feedTraceFromColumns(const QVector<QVector<float>> &columns, float binWidth)
{
    if (!m_traceWindow || !m_traceWindow->isVisible() || !m_traceWindow->hasTarget()) {
        return;
    }
    if (m_traceWindow->isFrozen() || m_traceWindow->isPlayingBack() || columns.isEmpty()) {
        return;
    }

    const float noise = m_spectrumWidget ? m_spectrumWidget->latestNoiseFloorDb() : -95.0f;
    float lastOffset = m_traceWindow->targetOffsetHz();

    for (const QVector<float> &col : columns) {
        if (col.isEmpty()) {
            continue;
        }
        /* AFC peak-lock + envelope sample inside processSpectrumColumn */
        lastOffset = m_traceWindow->processSpectrumColumn(col, binWidth, noise);
    }

    if (m_spectrumWidget && m_traceWindow->afcEnabled()) {
        m_spectrumWidget->setTraceOffsetHz(lastOffset);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_freezeDisplay = true;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    }
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait(8000);
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::onStartClicked()
{
    if (!m_worker) {
        return;
    }
    if (m_worker->isDetectionActive()) {
        QMessageBox::information(this, "Info", "Detector is already running");
        return;
    }

    m_freezeDisplay = false;
    m_trainingPlayback = false;
    m_isConnected = false;
    updateConnectionIndicator();
    /* Live radio. A chosen training file is used only by Start playback. */
    const QString noTrainingFile;
    QMetaObject::invokeMethod(m_worker, "setConfig", Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("training_file")),
                              Q_ARG(QString, noTrainingFile));
    /* Default to full 48 kHz wide waterfall (production path) */
    QMetaObject::invokeMethod(m_worker, "setConfig", Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("spectrum_span_hz")),
                              Q_ARG(QString, QStringLiteral("0")));
    m_spectrumModeLabel = QStringLiteral("WIDE");
    updateStatusBar();
#ifdef CONN_DEBUG
    fprintf(stderr, "[CONN] MainWindow: Start button clicked, queuing worker start (WIDE 48 kHz)\n");
    fflush(stderr);
#endif
    /* start() runs on the worker QThread and joins any leftover detection pthread */
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

void MainWindow::stopDetection(const QString &reason)
{
    if (!m_worker) {
        return;
    }

    m_freezeDisplay = true;
    m_isRunning = false;
    m_isConnected = false;
    /* Drop the speaker before flushing events. Deleting it while its
     * backend still has queued notifications crashes after a long run. */
    if (m_audioMonitor) {
        m_audioMonitor->setStreamActive(false, false);
    }
    m_statusLabel->setText("Status: STOPPING...");
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    /*
     * stop() MUST run on the worker thread: it joins the detection pthread and
     * owns spectrum buffers. Calling requestStop from the GUI thread races the
     * detection pthread and has caused Start→Stop→Start segfaults.
     */
    const bool ok = QMetaObject::invokeMethod(m_worker, "stop",
                                              Qt::BlockingQueuedConnection);
    if (!ok) {
        onLogMessage("Warning: could not invoke worker stop", 3);
    }

    if (m_multiDecoder) {
        m_multiDecoder->clear();
    }

    if (!reason.isEmpty()) {
        onLogMessage(reason, 1);
    }
    updateStatusBar();
}

void MainWindow::onStopClicked()
{
    stopDetection("Detection stopped");
}

void MainWindow::onSettingsClicked()
{
    /*
     * Build a fresh dialog each time. The old long-lived dialog used
     * QVBoxLayout(this) + setLayout() which can crash under Qt5 when re-shown.
     */
    SettingsDialog dialog(this);
    if (m_multiDecoder) {
        dialog.setDecodeChannels(m_multiDecoder->maxActiveChannels());
    }

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const int decodeCh = dialog.getDecodeChannels();

    if (m_worker) {
        m_worker->setConfig("radio_host", dialog.getRadioHost());
        m_worker->setConfig("radio_port", QString::number(dialog.getRadioPort()));
        m_worker->setConfig("radio_protocol", dialog.getRadioProtocol());
        m_worker->setConfig("detection_threshold",
                           QString::number(dialog.getDetectionThreshold()));
        m_worker->setConfig("min_snr_db",
                           QString::number(dialog.getMinSnrDb(), 'f', 1));
        m_worker->setConfig("spot_server_callsign", dialog.getCallsign());
        m_worker->setConfig("multi_decode_channels", QString::number(decodeCh));
    }
    if (m_multiDecoder) {
        m_multiDecoder->setMaxActiveChannels(decodeCh);
        onLogMessage(QString("CW Decode channels → %1").arg(decodeCh), 1);
    }
}

void MainWindow::onClearClicked()
{
    if (m_spectrumWidget) {
        m_spectrumWidget->clear();
    }
    if (m_decodeWidget) {
        m_decodeWidget->clear();
    }
    if (m_multiDecoder) {
        m_multiDecoder->clear();
    }
    if (m_morseWindow) {
        m_morseWindow->clearDecode();
    }
    if (m_maskWindow) {
        m_maskWindow->clearDecode();
    }
    if (m_scopeWindow) {
        m_scopeWindow->clearDecode();
        m_scopeWindow->resetTiming();
    }
}

void MainWindow::onSignalDetected(float frequency, float freqOffsetHz, float snr, float confidence,
                                   float tonePurity, float bandwidth)
{
    (void)confidence;
    (void)tonePurity;
    (void)bandwidth;
    if (m_spectrumWidget) {
        const float snappedOffset = m_spectrumWidget->snapOffsetHz(freqOffsetHz);
        frequency = snappedOffset + (frequency - freqOffsetHz);
        freqOffsetHz = snappedOffset;
        m_spectrumWidget->addSignal(frequency, freqOffsetHz, snr);
    }
}

void MainWindow::onSpotReported(QString callsign, float frequency, float snr, float confidence)
{
    (void)callsign;
    (void)frequency;
    (void)snr;
    (void)confidence;
}

void MainWindow::onStatsUpdated(int /*numSignals*/, float /*noiseFloor*/, float avgPeakSnr,
                                 float peakSnr, int spectrumPeakCount, int bufferFill,
                                 bool connected, int /*samplesProcessed*/, float cpuUsage,
                                 int queueSize)
{
    if (m_freezeDisplay) {
        return;
    }
    m_isConnected = connected;
    m_bufferFill = bufferFill;
    m_cpuUsage = cpuUsage;
    m_queueSize = queueSize;
    m_avgPeakSnr = avgPeakSnr;
    m_peakSnr = peakSnr;
    m_spectrumPeakCount = spectrumPeakCount;

    updateStatusBar();
}

void MainWindow::onSpectrumColumnsReady(QVector<QVector<float>> columns, float centerFreq,
                                        float binWidth)
{
    if (m_freezeDisplay || columns.isEmpty()) {
        return;
    }

    /* Follow radio VFO: update labels / status whenever center moves */
    if (centerFreq > 1.0e5f) {
        if (std::fabs(centerFreq - m_lastCenterFreqHz) > 5.0f) {
            if (m_lastCenterFreqHz > 1.0e5f) {
                onLogMessage(QString("Radio frequency now %1 MHz (was %2)")
                                 .arg(centerFreq / 1e6f, 0, 'f', 6)
                                 .arg(m_lastCenterFreqHz / 1e6f, 0, 'f', 6),
                             1);
            }
            m_lastCenterFreqHz = centerFreq;
            updateStatusBar();
        } else {
            m_lastCenterFreqHz = centerFreq;
        }
    }

    if (m_decodeWidget) {
        m_decodeWidget->setFrequencyScale(centerFreq, binWidth, columns.last().size());
    }
    m_spectrumBins = columns.last().size();
    m_spectrumBinWidth = binWidth;
    syncSpectrumScroll(m_spectrumBins, m_spectrumBinWidth);
    if (m_spectrumWidget) {
        m_spectrumWidget->appendSpectrumColumns(columns, centerFreq, binWidth);
    }
    /*
     * Each 2 kHz slice keeps its own Spectrum decoder.
     * The Decoder button only changes the bottom Morse pane.
     * Listen bandwidth matches CW Signal Trace "Listen bins ±" when open.
     */
    if (m_multiDecoder) {
        const int halfBins =
            (m_traceWindow && m_traceWindow->hasTarget()) ? m_traceWindow->halfBins() : 1;
        m_multiDecoder->setHalfBins(halfBins);
        const float batchNoise =
            m_spectrumWidget ? m_spectrumWidget->latestNoiseFloorDb() : -95.0f;
        for (const QVector<float> &col : columns) {
            if (col.isEmpty()) {
                continue;
            }
            const float noise = m_spectrumWidget
                                    ? m_spectrumWidget->estimateNoiseFloor(col)
                                    : batchNoise;
            m_multiDecoder->processSpectrumColumn(col, binWidth, centerFreq, noise);
        }
    }
    /* Feed single-signal scope even while main display is live */
    feedTraceFromColumns(columns, binWidth);

    /*
     * Spectrum decoder reads the waterfall pixels at the selected trace
     * frequency: white (lit) is key-down, black and near-black are key-up.
     */
    if (m_morseBackend == MorseDecoderBackend::Spectrum && m_scopeWindow
        && m_traceWindow && m_traceWindow->isVisible() && m_traceWindow->hasTarget()
        && m_spectrumWidget) {
        const float offsetHz = m_traceWindow->targetOffsetHz();
        const int halfBins = m_traceWindow->halfBins();
        for (const QVector<float> &col : columns) {
            if (col.isEmpty()) {
                continue;
            }
            const float noise = m_spectrumWidget->estimateNoiseFloor(col);
            const float power = SpectrumWidget::powerAtOffset(col, offsetHz, binWidth, halfBins);
            m_scopeWindow->feedColumn(power, noise, binWidth);
        }
        if (m_traceWindow) {
            m_scopeWindow->setTargetLabel(
                QString("IF %1 Hz").arg(offsetHz, 0, 'f', 0));
        }
    }
}

void MainWindow::onSpectrumModeWide()
{
    if (m_worker) {
        m_worker->setConfig(QStringLiteral("spectrum_span_hz"), QStringLiteral("0"));
    }
    m_spectrumModeLabel = QStringLiteral("WIDE");
    if (m_spectrumWidget) {
        m_spectrumWidget->clear();
    }
    onLogMessage("Spectrum mode → WIDE 48 kHz (default workable path, ~47 Hz bins)", 1);
    updateStatusBar();
}

void MainWindow::onSpectrumModeNarrow3k()
{
    if (m_worker) {
        m_worker->setConfig(QStringLiteral("spectrum_span_hz"), QStringLiteral("3000"));
    }
    m_spectrumModeLabel = QStringLiteral("NARROW 3k");
    if (m_spectrumWidget) {
        m_spectrumWidget->clear();
    }
    onLogMessage(
        "Spectrum mode → NARROW 3 kHz experiment (±1.5 kHz around VFO, ~12 Hz bins, "
        "~11 ms columns). Tune radio so CW is near center; use Wide to go back.",
        1);
    updateStatusBar();
}

void MainWindow::onDecodeUpdated(QString decodedText, float frequencyHz,
                                 float freqOffsetHz, float confidence)
{
    if (m_freezeDisplay) {
        return;
    }
    if (m_decodeWidget) {
        m_decodeWidget->appendDecode(decodedText, frequencyHz,
                                     freqOffsetHz, confidence);
    }
}

void MainWindow::onLogMessage(QString message, int level)
{
    /* Logs tab removed — keep messages on stderr / status path */
    static const char *const names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    const char *tag = (level >= 0 && level <= 3) ? names[level] : "LOG";
    fprintf(stderr, "[%s] %s\n", tag, qPrintable(message));
    fflush(stderr);
}

void MainWindow::updateMonitorChannel()
{
    const bool selected = m_spectrumWidget && m_spectrumWidget->hasTraceSelection() &&
                          m_traceWindow && m_traceWindow->hasTarget() &&
                          m_traceWindow->isVisible();
    if (m_audioMonitor) {
        m_audioMonitor->setStreamActive(m_isRunning, selected);
    }
}

void MainWindow::onWorkerStatusChanged(bool running)
{
    m_isRunning = running;
    if (running) {
        m_freezeDisplay = false;
        updateMonitorChannel();
        updateStatusBar();
    } else {
        if (m_trainingPlayback && !m_freezeDisplay) {
            onLogMessage(QStringLiteral("Training playback finished"), 1);
        }
        m_trainingPlayback = false;
        updateMonitorChannel();
        if (!m_freezeDisplay) {
            updateStatusBar();
        }
    }
}

void MainWindow::onWorkerError(QString error)
{
    QMessageBox::critical(this, "Error", error);
    onLogMessage(error, 3);  // Level 3 = ERROR
}

void MainWindow::onSignalTraceRequested(float freqOffsetHz, float absFreqHz)
{
    ensureTraceWindow();
    if (!m_traceWindow || !m_spectrumWidget) {
        return;
    }

    const float binWidth = m_spectrumWidget->binWidth();
    m_traceWindow->setTarget(freqOffsetHz, absFreqHz, binWidth);

    /* Seed with waterfall history so the scope isn't empty on open */
    const QVector<float> history =
        m_spectrumWidget->powerHistoryAtOffset(freqOffsetHz, m_traceWindow->halfBins());
    m_traceWindow->seedHistory(history, m_spectrumWidget->latestNoiseFloorDb());

    m_traceWindow->show();
    m_traceWindow->raise();
    m_traceWindow->activateWindow();

    const QString sideCopy = m_multiDecoder ? m_multiDecoder->textForOffset(freqOffsetHz)
                                            : QString();
    if (m_morseWindow) {
        m_morseWindow->adoptText(sideCopy);
        m_morseWindow->setThresholdDb(m_traceWindow->thresholdDb());
        m_morseWindow->setTargetLabel(
            QString("IF %1 Hz").arg(m_traceWindow->targetOffsetHz(), 0, 'f', 0));
    }
    if (m_maskWindow) {
        m_maskWindow->adoptText(sideCopy);
        m_maskWindow->setTargetLabel(
            QString("IF %1 Hz").arg(m_traceWindow->targetOffsetHz(), 0, 'f', 0));
    }
    if (m_scopeWindow) {
        m_scopeWindow->adoptText(sideCopy);
        m_scopeWindow->setTargetLabel(
            QString("IF %1 Hz").arg(m_traceWindow->targetOffsetHz(), 0, 'f', 0));
    }

    onLogMessage(QString("Signal trace: offset %1 Hz (RF %2 Hz)")
                     .arg(freqOffsetHz, 0, 'f', 1)
                     .arg(absFreqHz, 0, 'f', 0),
                 1);
    updateMonitorChannel();
}

void MainWindow::onTraceWindowClosed()
{
    if (m_spectrumWidget) {
        m_spectrumWidget->clearTraceSelection();
    }
    updateMonitorChannel();
}

void MainWindow::onTraceTuningChanged()
{
    reseedTraceFromWaterfall();
    /* Retune resets only the bottom single-signal Morse panel — not multi CW Decode */
    if (m_morseWindow && m_traceWindow) {
        m_morseWindow->resetTiming();
        m_morseWindow->setTargetLabel(
            QString("IF %1 Hz").arg(m_traceWindow->targetOffsetHz(), 0, 'f', 0));
    }
    if (m_maskWindow && m_traceWindow) {
        m_maskWindow->resetTiming();
        m_maskWindow->setTargetLabel(
            QString("IF %1 Hz").arg(m_traceWindow->targetOffsetHz(), 0, 'f', 0));
    }
    if (m_multiDecoder && m_traceWindow) {
        m_multiDecoder->setHalfBins(m_traceWindow->halfBins());
    }
    if (m_spectrumWidget && m_traceWindow && m_traceWindow->hasTarget()) {
        /* Keep waterfall green marker aligned with fine-tuned offset */
        m_spectrumWidget->setTraceOffsetHz(m_traceWindow->targetOffsetHz());
    }
    updateMonitorChannel();
}

void MainWindow::onTraceThresholdChanged(float thresholdDb)
{
    /*
     * Scope thr only drives the bottom Morse Decoder panel (single-signal UI).
     * Multi-channel CW Decode engines keep their own Auto thr and are not updated.
     */
    if (m_morseWindow) {
        m_morseWindow->setThresholdDb(thresholdDb);
    }
    if (m_maskWindow) {
        m_maskWindow->setThresholdDb(thresholdDb);
    }
}

void MainWindow::onTraceSampleReady(float powerDb, bool aboveThreshold, qint64 sampleTimeMs)
{
    /*
     * Single-signal scope → bottom Morse Decoder panel only.
     * CW Decode multi-channel path is fed from spectrum columns, not here.
     */
    if (m_morseBackend == MorseDecoderBackend::Spectrum) {
        return;
    }
    if (m_morseBackend == MorseDecoderBackend::Mask) {
        if (!m_maskWindow) {
            return;
        }
        m_maskWindow->feedSample(powerDb, aboveThreshold, sampleTimeMs);
        return;
    }

    if (!m_morseWindow) {
        return;
    }
    m_morseWindow->feedSample(powerDb, aboveThreshold, sampleTimeMs);
}

void MainWindow::syncSpectrumScroll(int numBins, float binWidth)
{
    if (!m_spectrumScroll || numBins <= 1 || binWidth <= 0.0f) {
        return;
    }
    const int fullSpanHz = std::max(1, static_cast<int>(std::lround(numBins * binWidth)));
    const int viewSpanHz = std::min(10000, fullSpanHz);
    const int fullLowHz = -fullSpanHz / 2;
    const int maxScroll = std::max(0, fullSpanHz - viewSpanHz);
    const int maxLow = fullLowHz + maxScroll;
    int viewLow = m_spectrumViewSet ? static_cast<int>(std::lround(m_spectrumViewLow))
                                    : (-viewSpanHz / 2);
    viewLow = std::max(fullLowHz, std::min(maxLow, viewLow));
    m_spectrumViewSet = true;
    m_spectrumScroll->setEnabled(maxScroll > 0);
    m_spectrumScroll->blockSignals(true);
    m_spectrumScroll->setRange(0, maxScroll);
    m_spectrumScroll->setPageStep(std::max(1, viewSpanHz));
    m_spectrumScroll->setSingleStep(std::max(1, viewSpanHz / 20));
    m_spectrumScroll->setValue(maxLow - viewLow);
    m_spectrumScroll->blockSignals(false);
    applySpectrumView();
}

void MainWindow::applySpectrumView()
{
    if (!m_spectrumScroll || m_spectrumBins <= 1 || m_spectrumBinWidth <= 0.0f) {
        return;
    }
    const int fullSpanHz = std::max(1, static_cast<int>(std::lround(
                                            m_spectrumBins * m_spectrumBinWidth)));
    const int viewSpanHz = std::min(10000, fullSpanHz);
    const int fullLowHz = -fullSpanHz / 2;
    const int maxScroll = std::max(0, fullSpanHz - viewSpanHz);
    const int maxLow = fullLowHz + maxScroll;
    const int viewLow = maxLow - m_spectrumScroll->value();
    m_spectrumViewLow = static_cast<float>(viewLow);
    if (m_spectrumWidget) {
        m_spectrumWidget->setFrequencyView(m_spectrumViewLow, static_cast<float>(viewSpanHz));
    }
    if (m_decodeWidget) {
        m_decodeWidget->setFrequencyView(m_spectrumViewLow, static_cast<float>(viewSpanHz));
    }
}

void MainWindow::onToggleMorseBackend()
{
    if (m_morseBackend == MorseDecoderBackend::Threshold) {
        m_morseBackend = MorseDecoderBackend::Mask;
        onLogMessage(
            "Morse backend → Mask (dit/dah boxes on Signal Trace scope).",
            1);
    } else if (m_morseBackend == MorseDecoderBackend::Mask) {
        m_morseBackend = MorseDecoderBackend::Spectrum;
        if (m_traceWindow) {
            m_traceWindow->setMaskOverlay(false);
        }
        if (m_scopeWindow) {
            m_scopeWindow->resetTiming();
            m_scopeWindow->clearDecode();
        }
        onLogMessage(
            "Morse backend → Spectrum (white trace on the waterfall, "
            "copied 6 dahs behind the leading edge).",
            1);
    } else {
        m_morseBackend = MorseDecoderBackend::Threshold;
        if (m_traceWindow) {
            m_traceWindow->setMaskOverlay(false);
        }
        onLogMessage("Morse backend → Threshold (mark/space + Auto WPM).", 1);
    }
    updateMorseBackendButton();
    showActiveMorsePanel();
}

QString MainWindow::trainingBrowseDirectory() const
{
    if (!m_trainingFile.isEmpty()) {
        const QString dir = QFileInfo(m_trainingFile).absolutePath();
        if (QDir(dir).exists()) {
            return dir;
        }
    }

    const QDir cwdTraining(QStringLiteral("training"));
    if (cwdTraining.exists()) {
        return cwdTraining.absolutePath();
    }

    QDir base(QCoreApplication::applicationDirPath());
    if (base.dirName() == QStringLiteral("bin")) {
        base.cdUp();
    }
    const QString besideApp = base.filePath(QStringLiteral("training"));
    if (QDir(besideApp).exists()) {
        return besideApp;
    }
    return QDir::currentPath();
}

void MainWindow::setTrainingFile(const QString &path)
{
    m_trainingFile = path;
    if (!m_trainingFileButton || !m_playTrainingButton) {
        return;
    }
    if (path.isEmpty()) {
        m_trainingFileButton->setText(QStringLiteral("Training file…"));
        m_trainingFileButton->setToolTip(
            QStringLiteral("Choose a .tcistream training file. This does not start playback."));
        m_playTrainingButton->setEnabled(false);
        return;
    }
    const QFileInfo info(path);
    m_trainingFileButton->setText(QStringLiteral("Training: %1").arg(info.fileName()));
    m_trainingFileButton->setToolTip(info.absoluteFilePath());
    m_playTrainingButton->setEnabled(true);
}

void MainWindow::onChooseTrainingFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Choose training file"),
        trainingBrowseDirectory(),
        QStringLiteral("TCI training streams (*.tcistream);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    setTrainingFile(path);
    onLogMessage(QStringLiteral("Training file selected: %1").arg(path), 1);
}

void MainWindow::onStartPlaybackClicked()
{
    if (!m_worker) {
        return;
    }
    if (m_trainingFile.isEmpty() || !QFileInfo::exists(m_trainingFile)) {
        QMessageBox::information(this, QStringLiteral("Training"),
                                 QStringLiteral("Choose a training file first."));
        return;
    }
    if (m_worker->isDetectionActive()) {
        QMessageBox::information(this, QStringLiteral("Training"),
                                 QStringLiteral("Stop the detector before starting playback."));
        return;
    }

    m_freezeDisplay = false;
    m_trainingPlayback = true;
    onLogMessage(QStringLiteral("Training playback: %1").arg(m_trainingFile), 1);
    QMetaObject::invokeMethod(m_worker, "setConfig", Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("training_file")),
                              Q_ARG(QString, m_trainingFile));
    QMetaObject::invokeMethod(m_worker, "setConfig", Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("spectrum_span_hz")),
                              Q_ARG(QString, QStringLiteral("0")));
    m_spectrumModeLabel = QStringLiteral("WIDE");
    updateStatusBar();
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

void MainWindow::onPlayCapture()
{
    ensureTraceWindow();
    if (!m_traceWindow) {
        return;
    }
    m_traceWindow->show();
    m_traceWindow->raise();
    m_traceWindow->activateWindow();
    /* Open file dialog on the scope window (same as its Play button) */
    QMetaObject::invokeMethod(m_traceWindow, "onPlayCaptureClicked");
}

void MainWindow::onPlaybackStarted(const QString &path)
{
    const QString playLabel = QString("PLAY %1").arg(QFileInfo(path).fileName());

    if (m_morseBackend == MorseDecoderBackend::Mask) {
        if (m_maskWindow) {
            if (m_traceWindow) {
                m_maskWindow->setThresholdDb(m_traceWindow->thresholdDb());
                m_maskWindow->setNoiseFloorDb(m_traceWindow->thresholdDb() - 15.0f);
            }
            m_maskWindow->setTargetLabel(playLabel);
            m_maskWindow->prepareForPlayback();
            showActiveMorsePanel();
            onLogMessage(
                QString("Capture playback: %1 — Mask dit/dah trapezoid decode")
                    .arg(QFileInfo(path).fileName()),
                1);
            return;
        }
    }

    if (m_morseWindow) {
        if (m_traceWindow) {
            m_morseWindow->setThresholdDb(m_traceWindow->thresholdDb());
            m_morseWindow->setTargetLabel(playLabel);
        }
        /* Opens squelch + clears text — must run before samples arrive */
        m_morseWindow->prepareForPlayback();
        showActiveMorsePanel();
    }
    onLogMessage(
        QString("Capture playback: %1 — thr frozen; Morse squelch open; "
                "set Code speed 3–4 WPM (or Auto) for slow CQ")
            .arg(QFileInfo(path).fileName()),
        1);
}

void MainWindow::onPlaybackFinished()
{
    if (m_morseWindow) {
        m_morseWindow->setPlaybackMode(false);
    }
    if (m_maskWindow) {
        m_maskWindow->setPlaybackMode(false);
    }
    onLogMessage("Capture playback finished", 1);
}

void MainWindow::onRecordToggled(bool checked)
{
    ensureTraceWindow();
    if (!m_traceWindow) {
        if (m_recordButton) {
            m_recordButton->blockSignals(true);
            m_recordButton->setChecked(false);
            m_recordButton->blockSignals(false);
        }
        return;
    }
    if (checked && !m_traceWindow->hasTarget()) {
        onLogMessage(
            "Record: click a signal on the waterfall first (opens Signal Trace target).",
            2);
        /* Still arm recording so samples are kept once a target is chosen. */
    }
    m_traceWindow->setRecordingActive(checked);
}

void MainWindow::onRecordingActiveChanged(bool active)
{
    if (!m_recordButton) {
        return;
    }
    m_recordButton->blockSignals(true);
    m_recordButton->setChecked(active);
    if (active) {
        m_recordButton->setText(QStringLiteral("Recording…"));
        m_recordButton->setStyleSheet(
            QStringLiteral("QPushButton { background-color: #803030; color: white; }"));
    } else {
        m_recordButton->setText(QStringLiteral("Record"));
        m_recordButton->setStyleSheet(QString());
    }
    m_recordButton->blockSignals(false);
}

void MainWindow::onRecordingStoppedForSave()
{
    if (!m_traceWindow) {
        return;
    }

    /* Keep the scope on screen — never use a modal dialog here (it was
       dismissing the scope window when OK was pressed). */
    m_traceWindow->show();
    m_traceWindow->raise();

    if (m_traceWindow->captureSampleCount() < 2) {
        m_traceWindow->setStatusNotice(
            "Recording stopped — not enough samples to save. Toggle Record on, key, then off.");
        onLogMessage("Scope recording stopped with too few samples", 2);
        m_traceWindow->activateWindow();
        return;
    }

    const QString dir = captureDirectory();
    QDir().mkpath(dir);
    const QString base = QString("%1/cwtrace_%2_f%3")
                             .arg(dir,
                                  QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"))
                             .arg(static_cast<int>(m_traceWindow->targetAbsFreqHz()));

    fprintf(stderr, "[TRACE-SAVE] dir=%s samples=%d base=%s\n",
            qPrintable(dir), m_traceWindow->captureSampleCount(), qPrintable(base));
    fflush(stderr);

    if (m_traceWindow->saveCapture(base)) {
        const QString notice =
            QString("SAVED (scope frozen for hand decode): %1.wav  +  %1.cwtrace  (%2 samples)")
                .arg(base)
                .arg(m_traceWindow->captureSampleCount());
        m_traceWindow->setStatusNotice(notice);
        onLogMessage(QString("Saved scope capture %1.cwtrace / .wav (%2 samples)")
                         .arg(base)
                         .arg(m_traceWindow->captureSampleCount()),
                     1);
        fprintf(stderr, "[TRACE-SAVE] OK %s.cwtrace %s.wav\n",
                qPrintable(base), qPrintable(base));
        fflush(stderr);
    } else {
        m_traceWindow->setStatusNotice(
            QString("SAVE FAILED — could not write %1.cwtrace / .wav").arg(base));
        onLogMessage(QString("Scope capture save failed: %1").arg(base), 3);
        fprintf(stderr, "[TRACE-SAVE] FAILED %s\n", qPrintable(base));
        fflush(stderr);
    }

    m_traceWindow->show();
    m_traceWindow->raise();
    m_traceWindow->activateWindow();
}

void MainWindow::onCaptureMarkRequested(float freqOffsetHz, float absFreqHz)
{
    if (m_captureInProgress) {
        return;
    }

    if (!m_worker || !m_worker->isDetectionActive()) {
        QMessageBox::information(this, "Capture",
                                 "Start detection first, then Shift+click a signal on the waterfall.");
        return;
    }

    const QString freqLabel = QString("%1.%2 MHz (offset %3 Hz)")
                                  .arg(static_cast<int>(absFreqHz / 1000000))
                                  .arg(static_cast<int>((absFreqHz / 1000.0)) % 1000, 3, 10, QChar('0'))
                                  .arg(static_cast<int>(freqOffsetHz));

    m_captureInProgress = true;
    m_freezeDisplay = true;
    m_worker->pauseGuiUpdates();

    if (!m_worker->freezeCapture()) {
        m_captureInProgress = false;
        m_freezeDisplay = false;
        m_worker->resumeGuiUpdates();
        QMessageBox::warning(
            this, "Capture Not Ready",
            "No IQ data in the 20-second buffer yet.\n\n"
            "Let detection run for a few seconds, then Shift+click again.");
        return;
    }

    m_statusLabel->setText(QString("Status: CAPTURE — frozen at %1").arg(freqLabel));
    onLogMessage(QString("Capture snapshot at %1 (offset %2 Hz)")
                     .arg(freqLabel)
                     .arg(freqOffsetHz, 0, 'f', 1),
                 1);

    raise();
    activateWindow();

    QInputDialog dialog(this);
    dialog.setWindowTitle("Save Capture");
    dialog.setLabelText(
        QString("Waterfall frozen at click time. Detection keeps running in the background.\n\n"
                "Marked frequency: %1\n\n"
                "What should the decoder find? Enter the text you see/hear\n"
                "(e.g. CQ, CQ DE N9CK, TEST). This is saved in the capture file\n"
                "for offline replay testing.\n\n"
                "Optional notes after a semicolon: CQ DE N9CK; ~20 WPM strong trace")
            .arg(freqLabel));
    dialog.setTextValue("CQ");
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint, true);

    const bool accepted = (dialog.exec() == QDialog::Accepted);
    const QString rawInput = dialog.textValue().trimmed();

    auto finishCapture = [this]() {
        if (m_worker && m_worker->isDetectionActive()) {
            m_worker->resumeGuiUpdates();
            m_freezeDisplay = false;
        }
        m_captureInProgress = false;
        updateStatusBar();
    };

    if (!accepted || rawInput.isEmpty()) {
        m_worker->clearFrozenCapture();
        onLogMessage("Capture cancelled", 2);
        finishCapture();
        return;
    }

    QString expected = rawInput;
    QString notes;
    const int semi = rawInput.indexOf(';');
    if (semi >= 0) {
        expected = rawInput.left(semi).trimmed();
        notes = rawInput.mid(semi + 1).trimmed();
    }
    if (expected.isEmpty()) {
        m_worker->clearFrozenCapture();
        onLogMessage("Capture cancelled (no expected decode text)", 2);
        finishCapture();
        return;
    }

    const QString path = QFileInfo(
        QString("%1/cwcap_%2_f%3.cwcap")
            .arg(captureDirectory(),
                 QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"))
            .arg(static_cast<int>(absFreqHz))).absoluteFilePath();

    const bool saved = m_worker->saveCapture(path, freqOffsetHz, expected, notes);

    if (saved) {
        QMessageBox msg(this);
        msg.setWindowTitle("Capture Saved");
        msg.setIcon(QMessageBox::Information);
        msg.setText(QString("Saved replay capture:\n%1\n\nExpected decode: %2")
                        .arg(path, expected));
        msg.setInformativeText(QString("Replay: make replay CAPTURE=%1").arg(path));
        msg.setWindowFlag(Qt::WindowStaysOnTopHint, true);
        msg.exec();
        onLogMessage(QString("Saved capture %1 expect='%2' mark=%3 Hz")
                           .arg(path, expected)
                           .arg(freqOffsetHz, 0, 'f', 1),
                       1);
    } else {
        m_worker->clearFrozenCapture();
        QMessageBox msg(this);
        msg.setWindowTitle("Capture Failed");
        msg.setIcon(QMessageBox::Warning);
        msg.setText(QString("Could not save capture to:\n%1").arg(path));
        msg.setInformativeText(
            "The frozen snapshot was empty or could not be written to disk.");
        msg.setWindowFlag(Qt::WindowStaysOnTopHint, true);
        msg.exec();
        onLogMessage(QString("Capture failed: %1").arg(path), 3);
    }

    finishCapture();
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, "About CW Skimmer",
                       "CW Skimmer for Linux\n\n"
                       "A high-performance CW signal detector and decoder\n"
                       "using Bayesian machine learning.\n\n"
                       "Version 1.0");
}

void MainWindow::updateConnectionIndicator()
{
    const bool on = m_isRunning && m_isConnected;
    const QString text = on ? QStringLiteral("●  Connected")
                            : QStringLiteral("●  Not connected");
    const QString style = on
        ? QStringLiteral(
              "QLabel { background-color: #1b5e20; color: #e8f5e9;"
              " border-radius: 4px; padding: 3px 10px; font-weight: bold; }")
        : QStringLiteral(
              "QLabel { background-color: #4a1515; color: #ffcdd2;"
              " border-radius: 4px; padding: 3px 10px; font-weight: bold; }");

    if (m_connectLamp) {
        m_connectLamp->setText(text);
        m_connectLamp->setStyleSheet(style);
    }
    if (m_connectionLabel) {
        m_connectionLabel->setText(text);
        m_connectionLabel->setStyleSheet(style);
    }
}

void MainWindow::updateStatusBar()
{
    QString statusText = m_isRunning ? "Status: RUNNING" : "Status: STOPPED";
    if (m_lastCenterFreqHz > 1.0e5f) {
        statusText += QString("  RF %1 MHz")
                          .arg(m_lastCenterFreqHz / 1e6f, 0, 'f', 6);
    }
    if (!m_spectrumModeLabel.isEmpty()) {
        statusText += QString("  [%1]").arg(m_spectrumModeLabel);
    }
    statusText += (m_tciStreamMode == TciStreamMode::Audio)
                      ? QStringLiteral("  [Stream Audio]")
                      : QStringLiteral("  [Stream IQ]");
    m_statusLabel->setText(statusText);

    updateConnectionIndicator();

    m_bufferLabel->setText(QString("Buffer: %1").arg(m_bufferFill));
    m_cpuLabel->setText(QString("CPU: %1%").arg((int)m_cpuUsage));
    m_queueLabel->setText(QString("Queue: %1").arg(m_queueSize));

    if (m_isRunning && m_isConnected && m_spectrumPeakCount > 0) {
        m_snrLabel->setText(QString("Avg SNR: %1 dB  Peak: %2 dB")
                                .arg(m_avgPeakSnr, 0, 'f', 1)
                                .arg(m_peakSnr, 0, 'f', 1));
    } else if (m_isRunning && m_isConnected) {
        m_snrLabel->setText("Avg SNR: --");
    } else {
        m_snrLabel->setText("Avg SNR: --");
    }
}
