/**
 * @file audiomonitor.h
 * @brief Play the selected spectrum channel on a chosen output device.
 */

#ifndef AUDIOMONITOR_H
#define AUDIOMONITOR_H

#include <QObject>
#include <QAudioDeviceInfo>
#include <QByteArray>
#include <QList>

class QComboBox;
class QSlider;
class QTimer;
class QAudioOutput;
class QIODevice;

class AudioMonitor : public QObject {
    Q_OBJECT

public:
    explicit AudioMonitor(QObject *parent = nullptr);
    ~AudioMonitor() override;

    QComboBox *deviceCombo() const { return m_combo; }
    QSlider *volumeSlider() const { return m_volumeSlider; }

    /** Open the output only while detection is running and a channel is selected. */
    void setStreamActive(bool detectionRunning, bool channelSelected);

public slots:
    /** Key the sidetone from one newly drawn signal-trace sample. */
    void setTraceEnvelope(float powerDb, float thresholdDb);

private slots:
    void onDeviceChanged(int index);
    void onVolumeChanged(int percent);
    void onFeed();

private:
    void populateDevices();
    void openOutput();
    void closeOutput();
    void writeLevel(int sampleCount);

    QComboBox *m_combo;
    QSlider *m_volumeSlider;
    float m_volume;
    QList<QAudioDeviceInfo> m_devices;
    QTimer *m_feedTimer;
    QAudioOutput *m_output;
    QIODevice *m_io;
    int m_rate;
    int m_channels;
    double m_phase;
    float m_amp;
    float m_targetAmp;
    bool m_running;
    bool m_selected;
    bool m_closing;
};

#endif
