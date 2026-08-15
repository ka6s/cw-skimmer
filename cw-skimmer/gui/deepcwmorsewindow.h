/**
 * @file deepcwmorsewindow.h
 * @brief DeepCW (ONNX) Morse decoder window — fed from scope envelope samples
 *
 * Scope only provides power/threshold samples. We reconstruct a keyed tone
 * (OOK at 8 kHz, 800 Hz) and pass it to deepcw_engine, which resamples and
 * runs the CTC model.
 */

#ifndef DEEPCWMORSEWINDOW_H
#define DEEPCWMORSEWINDOW_H

#include <QWidget>
#include <QString>
#include <QElapsedTimer>
#include <QVector>

class QLabel;
class QPlainTextEdit;
class QPushButton;

struct deepcw_engine;

/** DeepCW neural Morse panel (embedded in main window). */
class DeepCwMorseWindow : public QWidget {
    Q_OBJECT

public:
    explicit DeepCwMorseWindow(QWidget *parent = nullptr);
    ~DeepCwMorseWindow() override;

    /** Load ONNX model if needed. Returns true when engine is ready. */
    bool ensureReady(QString *errorOut = nullptr);
    bool isReady() const { return m_ready; }

    void setTargetLabel(const QString &label);

    /**
     * Feed one envelope sample. Synthesizes audio from previous key state
     * for the elapsed interval, then runs DeepCW when enough audio buffers.
     * @param sampleTimeMs  ≥ 0 playback time; < 0 wall clock
     */
    void feedSample(float powerDb, bool aboveThreshold, qint64 sampleTimeMs = -1);

    void clearDecode();
    void resetEngine();

    void prepareForPlayback();
    void setPlaybackMode(bool on);

private slots:
    void onClearClicked();
    void onResetClicked();

private:
    void updateUi();
    void refreshTextView();
    void trimScrollBuffer();
    void synthesizeAndFeed(bool keyDown, double durationSec);
    void pullDecodedText();
    static QString resolveModelPath();

    QLabel *m_statusLabel;
    QPlainTextEdit *m_textView;
    QPushButton *m_clearButton;
    QPushButton *m_resetButton;

    deepcw_engine *m_engine;
    bool m_ready;
    bool m_globalInited;

    bool m_hasState;
    bool m_isHigh;
    qint64 m_lastSampleMs;
    QElapsedTimer m_clock;

    QString m_scrollText;
    QString m_targetLabel;
    QString m_lastPlain;
    QString m_statusMsg;

    float m_lastPowerDb;
    int m_sampleCount;
    bool m_playbackMode;

    double m_phase;  /* tone oscillator phase */

    static const int kSynthRate = 8000;
    static const float kToneHz;
    static const int kMaxScrollChars = 2000;
    static const int kMaxChunkSamples = 48000; /* 6 s safety cap per feed */
};

#endif // DEEPCWMORSEWINDOW_H
