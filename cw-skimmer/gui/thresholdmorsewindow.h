/**
 * @file thresholdmorsewindow.h
 * @brief Append-only Morse decode from threshold high/low stream
 */

#ifndef THRESHOLDMORSEWINDOW_H
#define THRESHOLDMORSEWINDOW_H

#include <QWidget>
#include <QString>
#include <QVector>
#include <QElapsedTimer>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QSlider;

/**
 * Streaming Morse decoder panel (embedded in main window):
 *  - Emits characters once and never rewrites history
 *  - Scrolling text window (oldest lines drop off)
 *  - CW squelch until a real strong keyed signal is present
 *  - Auto WPM from recent marks only (affects future symbols, not past text)
 *  - T/E spam hint when WPM/noise is wrong
 */
class ThresholdMorseWindow : public QWidget {
    Q_OBJECT

public:
    /**
     * @param headless  true = no UI (for multi-channel parallel decode)
     */
    explicit ThresholdMorseWindow(QWidget *parent = nullptr, bool headless = false);

    void setTargetLabel(const QString &label);
    void setThresholdDb(float thresholdDb);

    /**
     * @param sampleTimeMs  ≥ 0: absolute sample time (playback). < 0: use wall clock.
     */
    void feedSample(float powerDb, bool aboveThreshold, qint64 sampleTimeMs = -1);

    void clearDecode();
    void resetTiming();

    /**
     * Call when starting .cwtrace/.wav playback: reset decode state and open
     * squelch so offline captures always produce text (live RF still uses
     * energy-based squelch).
     */
    void prepareForPlayback();
    void setPlaybackMode(bool on);

    /** Full append-only decode string. */
    QString decodedText() const { return m_scrollText; }
    /** Last n characters (for multi-channel display strip). */
    QString trailingText(int maxChars) const;

    int wpm() const;
    double unitSeconds() const { return m_unitSeconds; }
    bool isHeadless() const { return m_headless; }

signals:
    /** Emitted when the committed decode text changes. */
    void textChanged(const QString &text);

private slots:
    void onClearClicked();
    void onResetTimingClicked();
    void onWpmSliderChanged(int wpm);
    void onAutoToggled(bool checked);

private:
    void applyWpmFromSlider();
    void processStateChange(bool nowHigh, qint64 nowMs);
    void handleMarkEnd(double seconds);
    void handleSpaceEnd(double seconds);
    void checkLetterTimeout(qint64 nowMs);
    void appendChar(QChar ch);
    void flushPendingLetter();
    void commitPendingMarkToLetter();
    /**
     * While key is high, detect amplitude valleys (element gaps inside a
     * glued mark). Splits O (---) when thr keying never drops between dahs.
     * Capture OA (cwtrace_20260801_155432): O was one 1139 ms mark.
     */
    void processIntraMarkValley(float powerDb, qint64 nowMs);
    /** Commit mark + synthetic element-class space, resume mark after valley. */
    void splitMarkAtValley(qint64 valleyStartMs, qint64 nowMs);
    void updateSquelch(float powerDb, bool above, qint64 nowMs);
    void noteMarkDuration(double markSeconds);
    void noteLetterGap(double gapSeconds);
    void noteElementGap(double gapSeconds);
    void noteMarkPeak(float peakDb);
    void recomputeUnitFromMarks();
    double wordMinSeconds() const;
    void maybeInsertProtocolSpace(QChar nextChar);
    QString pendingMorsePreview() const;
    void trimScrollBuffer();
    void refreshTextView();
    void updateUi();
    static char morseToChar(const QString &pattern);

    bool m_headless;

    QLabel *m_statusLabel;
    QLabel *m_elementLabel;
    QLabel *m_timingLabel;
    QLabel *m_wpmLabel;
    QPlainTextEdit *m_textView;
    QPushButton *m_clearButton;
    QPushButton *m_resetTimingButton;
    QCheckBox *m_autoWpmCheck;
    QSlider *m_wpmSlider;

    bool m_hasState;
    bool m_isHigh;
    qint64 m_stateStartMs;
    qint64 m_lastSampleMs;
    QElapsedTimer m_clock;

    /* Current letter as mark durations — classified only at flush with latest unit */
    QVector<double> m_pendingMarkDurs;
    QVector<float> m_pendingMarkPeaks; /* peak power_db per pending mark */
    QString m_scrollText;       /* committed characters (append-only) */
    QString m_targetLabel;
    float m_thresholdDb;

    double m_unitSeconds;
    double m_sliderUnitSeconds;
    double m_pendingMarkSec;    /* mark fragment awaiting hole-merge */
    float m_pendingMarkPeakDb;  /* peak power during current mark fragment */
    bool m_havePendingMark;
    bool m_letterEmittedForSpace;
    bool m_unitCalibrated;      /* true once we have enough marks for clustering */

    /* Intra-mark valley split (element gap while Schmitt key stays high) */
    bool m_inValley;
    qint64 m_valleyStartMs;
    float m_segPeakDb;          /* peak of current mark segment */

    float m_lastPowerDb;
    int m_sampleCount;
    int m_markCount;
    int m_spaceCount;

    /* CW squelch */
    bool m_squelchOpen;
    bool m_playbackMode;   /* true during capture replay — keep squelch open */
    double m_markEnergySec;
    qint64 m_lastStrongMarkMs;
    qint64 m_lastMarkEndMs;
    float m_peakAboveThreshDb;

    /* Recent committed chars for T/E spam detection */
    QString m_recentChars;
    int m_spamHintCount;

    /* All recent mark durations for bimodal dit/dah unit estimate */
    QVector<double> m_allMarkSecs;

    /* Recent letter-class gaps (spaces that closed a letter but were not words).
     * Used to place word threshold between letter and word spacing for fists
     * where word ≈ 6–7× unit and letter ≈ 4–5× (e.g. GAFD ERON TR). */
    QVector<double> m_letterGaps;

    /* Element gaps (~1 dit) kept inside a letter — strong Auto WPM cue */
    QVector<double> m_elementGaps;

    /* Recent accepted letter peak powers — drop weak trailing I/E vs main CQ */
    QVector<float> m_recentLetterPeaks;

    static const int kMaxScrollChars = 2000;
    static const int kMaxRecentChars = 24;
    static const int kMaxMarkHistory = 40;
    static const int kMaxLetterGaps = 12;
    static const int kMaxElementGaps = 24;
    static const int kMaxLetterPeaks = 12;
};

#endif // THRESHOLDMORSEWINDOW_H
