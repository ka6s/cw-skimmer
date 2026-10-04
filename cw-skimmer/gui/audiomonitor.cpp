/**
 * @file audiomonitor.cpp
 * @brief Output-device chooser and playback of one CW channel.
 */

#include "audiomonitor.h"

#include <QAudioFormat>
#include <QAudioOutput>
#include <QComboBox>
#include <QCoreApplication>
#include <QIODevice>
#include <QSlider>
#include <QTimer>

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
constexpr int kSourceRate = 48000;
constexpr float kSidetoneHz = 700.0f;
}

AudioMonitor::AudioMonitor(QObject *parent)
    : QObject(parent)
    , m_combo(new QComboBox())
    , m_volumeSlider(new QSlider(Qt::Horizontal))
    , m_volume(0.80f)
    , m_feedTimer(new QTimer(this))
    , m_output(nullptr)
    , m_io(nullptr)
    , m_rate(kSourceRate)
    , m_channels(1)
    , m_phase(0.0)
    , m_amp(0.0f)
    , m_targetAmp(0.0f)
    , m_running(false)
    , m_selected(false)
    , m_closing(false)
{
    m_combo->setToolTip(
        QStringLiteral("Speaker for the signal trace.\n"
                       "A 700 Hz tone follows the trace: loud on the marks,\n"
                       "quiet in the spaces."));
    m_combo->setMinimumWidth(180);
    m_combo->setMaximumWidth(320);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(80);
    m_volumeSlider->setFixedWidth(90);
    m_volumeSlider->setToolTip(QStringLiteral("Trace audio volume: 80%"));
    populateDevices();
    connect(m_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AudioMonitor::onDeviceChanged);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &AudioMonitor::onVolumeChanged);
    m_feedTimer->setInterval(20);
    connect(m_feedTimer, &QTimer::timeout, this, &AudioMonitor::onFeed);
}

void AudioMonitor::onVolumeChanged(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    m_volume = (float)percent / 100.0f;
    if (m_volumeSlider) {
        m_volumeSlider->setToolTip(QStringLiteral("Trace audio volume: %1%").arg(percent));
    }
    if (m_output) {
        m_output->setVolume(m_volume);
    }
}

AudioMonitor::~AudioMonitor()
{
    closeOutput();
}

void AudioMonitor::populateDevices()
{
    const QAudioDeviceInfo preferred = QAudioDeviceInfo::defaultOutputDevice();
    int preferredIndex = 0;

    m_combo->blockSignals(true);
    m_combo->clear();
    m_devices = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
    for (int i = 0; i < m_devices.size(); ++i) {
        m_combo->addItem(m_devices.at(i).deviceName());
        if (m_devices.at(i).deviceName() == preferred.deviceName()) {
            preferredIndex = i;
        }
    }
    if (m_devices.isEmpty()) {
        m_combo->addItem(QStringLiteral("No output device"));
        m_combo->setEnabled(false);
    } else {
        m_combo->setEnabled(true);
        m_combo->setCurrentIndex(preferredIndex);
    }
    m_combo->blockSignals(false);
}

void AudioMonitor::setStreamActive(bool detectionRunning, bool channelSelected)
{
    if (m_running == detectionRunning && m_selected == channelSelected &&
        ((detectionRunning && channelSelected) == (m_output != nullptr))) {
        return;
    }
    m_running = detectionRunning;
    m_selected = channelSelected;
    if (m_running && m_selected) {
        openOutput();
    } else {
        closeOutput();
    }
}

void AudioMonitor::onDeviceChanged(int index)
{
    if (index < 0 || index >= m_devices.size()) {
        return;
    }
    if (m_running && m_selected) {
        openOutput();
    }
}

void AudioMonitor::openOutput()
{
    closeOutput();
    if (m_devices.isEmpty() || m_combo->currentIndex() < 0 ||
        m_combo->currentIndex() >= m_devices.size()) {
        return;
    }

    const QAudioDeviceInfo device = m_devices.at(m_combo->currentIndex());
    QAudioFormat format;
    format.setSampleRate(kSourceRate);
    format.setChannelCount(1);
    format.setSampleSize(16);
    format.setCodec(QStringLiteral("audio/pcm"));
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    if (!device.isFormatSupported(format)) {
        format = device.nearestFormat(format);
    }
    if (format.sampleSize() != 16 || format.sampleType() != QAudioFormat::SignedInt ||
        format.channelCount() < 1 || format.sampleRate() < 8000) {
        return;
    }

    m_rate = format.sampleRate();
    m_channels = format.channelCount();
    m_phase = 0.0;
    m_amp = 0.0f;
    m_targetAmp = 0.0f;

    /* Unparented: stop() must not delete this object out from under a queued event. */
    m_output = new QAudioOutput(device, format, nullptr);
    m_output->setBufferSize(m_rate * m_channels * 2 / 5);
    m_output->setVolume(m_volume);
    m_io = m_output->start();
    if (m_io) {
        m_feedTimer->start();
    }
}

void AudioMonitor::closeOutput()
{
    if (m_closing) {
        return;
    }
    m_closing = true;
    m_feedTimer->stop();

    QAudioOutput *output = m_output;
    m_output = nullptr;
    m_io = nullptr;
    m_phase = 0.0;
    m_amp = 0.0f;
    m_targetAmp = 0.0f;

    if (output) {
        output->blockSignals(true);
        output->stop();
        output->reset();
        if (!QCoreApplication::instance() || QCoreApplication::closingDown()) {
            delete output;
        } else {
            output->deleteLater();
        }
    }
    m_closing = false;
}

void AudioMonitor::setTraceEnvelope(float powerDb, float thresholdDb)
{
    if (m_closing || !m_io || !m_output ||
        !std::isfinite(powerDb) || !std::isfinite(thresholdDb)) {
        return;
    }

    /* Same threshold line drawn on the trace. A few dB of knee so edges aren't clicks. */
    const float rel = powerDb - thresholdDb;
    float target = (rel + 3.0f) / 6.0f;
    if (target < 0.0f) {
        target = 0.0f;
    } else if (target > 1.0f) {
        target = 1.0f;
    }
    m_targetAmp = target;
}

void AudioMonitor::onFeed()
{
    if (m_closing || !m_output || !m_io) {
        return;
    }

    const QAudio::Error err = m_output->error();
    QAudio::State state = m_output->state();
    /* Underrun is normal if the GUI stalled. Resume and keep sending. */
    if (err == QAudio::UnderrunError || state == QAudio::IdleState ||
        state == QAudio::SuspendedState) {
        m_output->resume();
        state = m_output->state();
    }
    if (state == QAudio::StoppedState) {
        m_io = m_output->start();
        if (!m_io) {
            return;
        }
    }

    int room = m_output->bytesFree();
    const int frame = m_channels * static_cast<int>(sizeof(qint16));
    if (room < frame) {
        /* Idle with no free space reported: push one slice so the stream restarts. */
        if (state != QAudio::IdleState && err != QAudio::UnderrunError) {
            return;
        }
        room = m_rate * frame * 20 / 1000;
    }
    room -= room % frame;
    if (room <= 0) {
        return;
    }
    /* Do not dump a huge catch-up block; 40 ms keeps the keying near the trace. */
    const int maxSamples = std::max(1, m_rate * 40 / 1000);
    int samples = room / frame;
    if (samples > maxSamples) {
        samples = maxSamples;
    }
    writeLevel(samples);
}

void AudioMonitor::writeLevel(int sampleCount)
{
    if (!m_io || sampleCount <= 0) {
        return;
    }
    QByteArray chunk(sampleCount * m_channels * static_cast<int>(sizeof(qint16)), '\0');
    qint16 *dst = reinterpret_cast<qint16 *>(chunk.data());
    const double dPhase = 2.0 * M_PI * static_cast<double>(kSidetoneHz) / static_cast<double>(m_rate);
    const float slew = std::min(1.0f, 8.0f / static_cast<float>(std::max(1, sampleCount)));

    for (int i = 0; i < sampleCount; ++i) {
        m_amp += (m_targetAmp - m_amp) * slew;
        const float sample = m_amp * m_volume * static_cast<float>(std::sin(m_phase));
        const int mixed = static_cast<int>(std::lround(sample * 26000.0f));
        const qint16 pcm = static_cast<qint16>(std::max(-32768, std::min(32767, mixed)));
        for (int c = 0; c < m_channels; ++c) {
            dst[i * m_channels + c] = pcm;
        }
        m_phase += dPhase;
        if (m_phase >= 2.0 * M_PI) {
            m_phase -= 2.0 * M_PI;
        }
    }
    m_io->write(chunk);
}
