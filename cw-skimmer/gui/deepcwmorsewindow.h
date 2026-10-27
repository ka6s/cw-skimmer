/**
 * @file deepcwmorsewindow.h
 * @brief Bottom-pane DeepCW decoder for the selected trace
 *
 * The model reads mono audio. Radio IQ is shifted so the clicked tone
 * sits in the middle of the model's band, then resampled.
 */

#ifndef DEEPCWMORSEWINDOW_H
#define DEEPCWMORSEWINDOW_H

#include <QString>
#include <QWidget>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QLabel;
class QPlainTextEdit;
struct DeepCwFront;

class DeepcwMorseWindow : public QWidget {
    Q_OBJECT

public:
    explicit DeepcwMorseWindow(QWidget *parent = nullptr);
    ~DeepcwMorseWindow() override;

    void warm();
    void setTargetLabel(const QString &label);
    void clearDecode();
    /** Drop filter and model state. Text already on screen stays. */
    void resetTiming();
    /** Keep this copy, then append whatever the model decodes next. */
    void adoptText(const QString &text);

    /**
     * pitch_hz is the true tone frequency. realAudio is demodulated
     * receiver audio (I holds the sample, Q is zero).
     */
    void setTune(double sampleRate, double pitchHz, bool realAudio);
    void feed(const float *interleavedIq, int complexCount);

    bool isTuned() const { return m_tuned; }
    QString decodedText() const { return m_shown; }

signals:
    void engineText(quint64 epoch, QString text);
    void engineNote(QString note);

private:
    void buildUi();
    void publish();
    void refreshStatus();
    bool locateModel();
    void ensureFront();
    void bumpEpoch();
    void engineMain(std::string modelPath, std::string metadataPath);

    QLabel *m_statusLabel;
    QPlainTextEdit *m_textView;
    DeepCwFront *m_front;

    QString m_prefix;
    QString m_engineText;
    QString m_shown;
    QString m_target;
    QString m_note;
    QString m_modelPath;
    QString m_metadataPath;

    bool m_tuned;
    bool m_realAudio;
    double m_sampleRate;
    double m_pitchHz;
    double m_targetRate;
    double m_centerHz;

    std::thread m_thread;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::vector<float> m_queue;
    bool m_stop;
    bool m_reset;
    bool m_started;
    std::atomic<quint64> m_epoch;
};

#endif
