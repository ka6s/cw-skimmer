/**
 * @file detectorworker.cpp
 * @brief Qt worker thread implementation
 */

#include "detectorworker.h"
#include <cstdio>
#include <cstring>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMutexLocker>

/* Verbose connect lifecycle — enable with -DCONN_DEBUG */
#ifdef CONN_DEBUG
#define CONN_LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#else
#define CONN_LOG(...) ((void)0)
#endif

DetectorWorker::DetectorWorker(const QString &configFile)
    : m_detector(nullptr)
    , m_configFile(configFile)
    , m_pendingCenterFreq(0.0f)
    , m_pendingBinWidth(46.875f)
    , m_spectrumBatchTimerActive(false)
    , m_detectThreadLaunched(false)
    , m_detectThreadActive(false)
    , m_acceptCallbacks(false)
{
    memset(&m_detectThread, 0, sizeof(m_detectThread));
}

DetectorWorker::~DetectorWorker()
{
    shutdown();
}

bool DetectorWorker::initialize()
{
    if (m_detector) {
        return false;
    }

    const QByteArray pathBytes = m_configFile.toUtf8();
    const char *configPath = pathBytes.isEmpty() ? nullptr : pathBytes.constData();
    m_detector = cwskimmer_detector_create(configPath);

    if (!m_detector) {
        emit errorOccurred(QStringLiteral("Failed to create detector"));
        return false;
    }

    cwskimmer_set_signal_callback(m_detector, signalCallbackStatic, this);
    cwskimmer_set_spot_callback(m_detector, spotCallbackStatic, this);
    cwskimmer_set_stats_callback(m_detector, statsCallbackStatic, this);
    cwskimmer_set_spectrum_callback(m_detector, spectrumCallbackStatic, this);
    cwskimmer_set_decode_callback(m_detector, decodeCallbackStatic, this);
    cwskimmer_set_log_callback(m_detector, logCallbackStatic, this);

    return true;
}

void DetectorWorker::shutdown()
{
    /* Prefer running stop on the worker thread when affinity allows. */
    if (QThread::currentThread() == thread()) {
        stop();
    } else if (thread() && thread()->isRunning()) {
        QMetaObject::invokeMethod(this, "stop", Qt::BlockingQueuedConnection);
    } else {
        m_acceptCallbacks = false;
        if (m_detector) {
            cwskimmer_stop(m_detector);
        }
        joinDetectionThread();
    }

    {
        QMutexLocker lock(&m_spectrumMutex);
        m_pendingSpectrumColumns.clear();
        m_spectrumBatchTimerActive = false;
    }

    if (m_detector) {
        cwskimmer_detector_destroy(m_detector);
        m_detector = nullptr;
        emit statusChanged(false);
    }
}

bool DetectorWorker::isRunning() const
{
    return m_detectThreadActive.load() ||
           (m_detector && cwskimmer_is_running(m_detector));
}

bool DetectorWorker::isDetectionActive() const
{
    return m_detectThreadActive.load() || m_detectThreadLaunched;
}

void DetectorWorker::joinDetectionThread()
{
    if (!m_detectThreadLaunched) {
        return;
    }
    CONN_LOG("[CONN] joining detection pthread...\n");
    fflush(stderr);
    pthread_join(m_detectThread, nullptr);
    m_detectThreadLaunched = false;
    m_detectThreadActive = false;
    memset(&m_detectThread, 0, sizeof(m_detectThread));
    CONN_LOG("[CONN] detection pthread joined\n");
    fflush(stderr);
}

bool DetectorWorker::waitForLoopInactive(int timeoutMs)
{
    if (!m_detector) {
        return true;
    }
    QElapsedTimer timer;
    timer.start();
    while (cwskimmer_is_loop_active(m_detector) && timer.elapsed() < timeoutMs) {
        QThread::msleep(20);
    }
    return !cwskimmer_is_loop_active(m_detector);
}

void *DetectorWorker::detectionThreadEntry(void *arg)
{
    static_cast<DetectorWorker *>(arg)->runDetectionLoop();
    return nullptr;
}

void DetectorWorker::runDetectionLoop()
{
    CONN_LOG("[CONN] detection pthread: entering cwskimmer_start\n");
    fflush(stderr);

    const int result = cwskimmer_start(m_detector);

    CONN_LOG("[CONN] detection pthread: cwskimmer_start returned %d\n", result);
    fflush(stderr);

    m_acceptCallbacks.store(false);
    m_detectThreadActive.store(false);

    /* Post to this QObject's thread (worker QThread), not the raw pthread. */
    QMetaObject::invokeMethod(this, [this, result]() {
        {
            QMutexLocker lock(&m_spectrumMutex);
            m_pendingSpectrumColumns.clear();
            m_spectrumBatchTimerActive = false;
        }
        emit statusChanged(false);
        if (result != 0) {
            emit errorOccurred(
                QStringLiteral("Failed to connect to radio — check host, port, protocol, "
                               "and that TCI is enabled"));
        }
    }, Qt::QueuedConnection);
}

void DetectorWorker::start()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "start", Qt::QueuedConnection);
        return;
    }

    if (!m_detector) {
        CONN_LOG("[CONN] DetectorWorker::start FAILED: detector not initialized\n");
        fflush(stderr);
        emit errorOccurred(QStringLiteral("Detector not initialized"));
        return;
    }

    if (m_detectThreadActive.load() || m_detectThreadLaunched) {
        if (m_detectThreadActive.load()) {
            CONN_LOG("[CONN] DetectorWorker::start ignored: already active\n");
            fflush(stderr);
            return;
        }
        /* Thread finished but not joined — join now. */
        joinDetectionThread();
    }

    if (cwskimmer_is_loop_active(m_detector) || cwskimmer_is_running(m_detector)) {
        CONN_LOG("[CONN] DetectorWorker::start: forcing stop of previous session\n");
        fflush(stderr);
        cwskimmer_stop(m_detector);
        if (!waitForLoopInactive(8000)) {
            emit errorOccurred(QStringLiteral(
                "Previous detection session still shutting down — try again in a moment"));
            return;
        }
    }

    /* Clear any leftover spectrum backlog before accepting new data */
    {
        QMutexLocker lock(&m_spectrumMutex);
        m_pendingSpectrumColumns.clear();
        m_spectrumBatchTimerActive = false;
    }

    m_acceptCallbacks.store(true);
    m_detectThreadActive.store(true);
    emit statusChanged(true);

    const int rc = pthread_create(&m_detectThread, nullptr, detectionThreadEntry, this);
    if (rc != 0) {
        m_acceptCallbacks.store(false);
        m_detectThreadActive.store(false);
        emit statusChanged(false);
        emit errorOccurred(QStringLiteral("Failed to start detection thread (error %1)").arg(rc));
        return;
    }
    m_detectThreadLaunched = true;
    CONN_LOG("[CONN] DetectorWorker::start: detection pthread created\n");
    fflush(stderr);
}

void DetectorWorker::stop()
{
    if (QThread::currentThread() != thread() && thread() && thread()->isRunning()) {
        QMetaObject::invokeMethod(this, "stop", Qt::BlockingQueuedConnection);
        return;
    }

    CONN_LOG("[CONN] DetectorWorker::stop begin\n");
    fflush(stderr);

    m_acceptCallbacks.store(false);
    {
        QMutexLocker lock(&m_spectrumMutex);
        m_pendingSpectrumColumns.clear();
        m_spectrumBatchTimerActive = false;
    }

    if (m_detector) {
        cwskimmer_stop(m_detector);
    }

    /* Wait for detection pthread to leave cwskimmer_start (includes cleanup). */
    if (m_detectThreadLaunched) {
        QElapsedTimer timer;
        timer.start();
        while (m_detectThreadActive.load() && timer.elapsed() < 10000) {
            QThread::msleep(20);
        }
        joinDetectionThread();
    }

    /* Extra safety: wait for C-side loop_active */
    if (m_detector && !waitForLoopInactive(3000)) {
        CONN_LOG("[CONN] WARNING: loop_active still set after join\n");
        fflush(stderr);
    }

    emit statusChanged(false);
    CONN_LOG("[CONN] DetectorWorker::stop done\n");
    fflush(stderr);
}

void DetectorWorker::setConfig(const QString &key, const QString &value)
{
    if (QThread::currentThread() != thread() && thread() && thread()->isRunning()) {
        QMetaObject::invokeMethod(this, "setConfig", Qt::QueuedConnection,
                                  Q_ARG(QString, key), Q_ARG(QString, value));
        return;
    }
    if (m_detector) {
        const QByteArray k = key.toUtf8();
        const QByteArray v = value.toUtf8();
        cwskimmer_config_set(m_detector, k.constData(), v.constData());
    }
}

void DetectorWorker::flushPendingSpectrum()
{
    QVector<QVector<float>> columns;
    float center = 0.0f;
    float binWidth = 46.875f;
    {
        QMutexLocker lock(&m_spectrumMutex);
        if (m_pendingSpectrumColumns.isEmpty()) {
            return;
        }
        columns.swap(m_pendingSpectrumColumns);
        center = m_pendingCenterFreq;
        binWidth = m_pendingBinWidth;
        m_spectrumBatchTimer.restart();
    }
    emit spectrumColumnsReady(columns, center, binWidth);
}

void DetectorWorker::signalCallbackStatic(const cwskimmer_signal_t *signal, void *userdata)
{
    auto *worker = static_cast<DetectorWorker *>(userdata);
    if (worker) {
        worker->onSignalDetected(signal);
    }
}

void DetectorWorker::spotCallbackStatic(const cwskimmer_spot_t *spot, void *userdata)
{
    auto *worker = static_cast<DetectorWorker *>(userdata);
    if (worker) {
        worker->onSpotReported(spot);
    }
}

void DetectorWorker::statsCallbackStatic(const cwskimmer_stats_t *stats, void *userdata)
{
    auto *worker = static_cast<DetectorWorker *>(userdata);
    if (worker) {
        worker->onStatsUpdated(stats);
    }
}

void DetectorWorker::spectrumCallbackStatic(const cwskimmer_spectrum_t *spectrum, void *userdata)
{
    auto *worker = static_cast<DetectorWorker *>(userdata);
    if (worker) {
        worker->onSpectrumUpdated(spectrum);
    }
}

void DetectorWorker::decodeCallbackStatic(const cwskimmer_decode_t *decode, void *userdata)
{
    auto *worker = static_cast<DetectorWorker *>(userdata);
    if (worker) {
        worker->onDecodeUpdated(decode);
    }
}

void DetectorWorker::logCallbackStatic(const char *message, int level, void *userdata)
{
    auto *worker = static_cast<DetectorWorker *>(userdata);
    if (worker) {
        worker->onLogMessage(message, level);
    }
}

void DetectorWorker::onSignalDetected(const cwskimmer_signal_t *signal)
{
    if (!m_acceptCallbacks.load() || !signal) {
        return;
    }
    emit signalDetected(signal->frequency, signal->freq_offset_hz, signal->snr_db,
                        signal->confidence, signal->tone_purity, signal->bandwidth);
}

void DetectorWorker::onSpotReported(const cwskimmer_spot_t *spot)
{
    if (!m_acceptCallbacks.load() || !spot) {
        return;
    }
    emit spotReported(QString::fromLatin1(spot->callsign), spot->frequency_hz,
                      spot->snr_db, spot->confidence);
}

void DetectorWorker::onStatsUpdated(const cwskimmer_stats_t *stats)
{
    if (!m_acceptCallbacks.load() || !stats) {
        return;
    }
    emit statsUpdated(stats->num_signals, stats->noise_floor_db,
                      stats->avg_peak_snr_db, stats->peak_snr_db,
                      stats->spectrum_peak_count, stats->buffer_fill,
                      stats->connected != 0, stats->samples_processed,
                      stats->cpu_usage, stats->queue_size);
}

void DetectorWorker::onSpectrumUpdated(const cwskimmer_spectrum_t *spectrum)
{
    if (!m_acceptCallbacks.load() || !spectrum || !spectrum->power_spectrum
        || spectrum->num_bins <= 0) {
        return;
    }

    QVector<float> column;
    column.resize(spectrum->num_bins);
    for (int i = 0; i < spectrum->num_bins; ++i) {
        column[i] = spectrum->power_spectrum[i];
    }

    bool shouldFlush = false;
    {
        QMutexLocker lock(&m_spectrumMutex);
        m_pendingSpectrumColumns.append(column);
        m_pendingCenterFreq = spectrum->center_frequency;
        m_pendingBinWidth = spectrum->bin_width;

        while (m_pendingSpectrumColumns.size() > 96) {
            m_pendingSpectrumColumns.removeFirst();
        }

        if (!m_spectrumBatchTimerActive) {
            m_spectrumBatchTimer.start();
            m_spectrumBatchTimerActive = true;
        }

        if (m_spectrumBatchTimer.elapsed() >= 20 || m_pendingSpectrumColumns.size() >= 8) {
            shouldFlush = true;
        }
    }

    if (shouldFlush) {
        flushPendingSpectrum();
    }
}

void DetectorWorker::onDecodeUpdated(const cwskimmer_decode_t *decode)
{
    if (!m_acceptCallbacks.load() || !decode) {
        return;
    }

    emit decodeUpdated(QString::fromLatin1(decode->text),
                       decode->frequency_hz, decode->freq_offset_hz,
                       decode->text_confidence);
}

void DetectorWorker::onLogMessage(const char *message, int level)
{
    if (!message) {
        return;
    }
    emit logMessage(QString::fromUtf8(message), level);
}

void DetectorWorker::pauseGuiUpdates()
{
    m_acceptCallbacks.store(false);
    QMutexLocker lock(&m_spectrumMutex);
    m_pendingSpectrumColumns.clear();
    m_spectrumBatchTimerActive = false;
}

void DetectorWorker::resumeGuiUpdates()
{
    if (m_detectThreadActive.load()) {
        m_acceptCallbacks.store(true);
        QMutexLocker lock(&m_spectrumMutex);
        m_spectrumBatchTimerActive = false;
    }
}

bool DetectorWorker::freezeCapture()
{
    if (!m_detector) {
        return false;
    }
    return cwskimmer_freeze_capture(m_detector) == 0;
}

void DetectorWorker::clearFrozenCapture()
{
    if (m_detector) {
        cwskimmer_clear_frozen_capture(m_detector);
    }
}

bool DetectorWorker::saveCapture(const QString &path, float markFreqOffsetHz,
                                 const QString &expectedText, const QString &notes)
{
    if (!m_detector || path.isEmpty()) {
        return false;
    }

    const QByteArray pathBytes = path.toUtf8();
    const QByteArray expectedBytes = expectedText.toUtf8();
    const QByteArray notesBytes = notes.toUtf8();
    return cwskimmer_save_capture(m_detector, pathBytes.constData(), markFreqOffsetHz,
                                  expectedBytes.constData(), notesBytes.constData()) == 0;
}
