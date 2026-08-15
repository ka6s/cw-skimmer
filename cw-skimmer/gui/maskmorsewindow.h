/**
 * @file maskmorsewindow.h
 * @brief Geometric dit/dah mask Morse decoder (pulse-fill)
 *
 * Threshold only finds the pulse (rising → falling key-high region).
 * Classification: does that pulse energy mostly fill the dit mask or the
 * dah mask? Widths from measured mark lengths or independent sliders.
 *
 * Visual: left-aligned dit/dah boxes on Signal Trace mid-scope DETECT —
 * red looking, green hit. Height = noise → peak.
 */

#ifndef MASKMORSEWINDOW_H
#define MASKMORSEWINDOW_H

#include <QWidget>
#include <QString>
#include <QVector>
#include <QElapsedTimer>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QSlider;

class MaskMorseWindow : public QWidget {
    Q_OBJECT

public:
    explicit MaskMorseWindow(QWidget *parent = nullptr, bool headless = false);
    ~MaskMorseWindow() override = default;

    void setTargetLabel(const QString &label);
    void setThresholdDb(float thresholdDb);
    void setNoiseFloorDb(float noiseFloorDb);

    void feedSample(float powerDb, bool aboveThreshold, qint64 sampleTimeMs = -1);

    void clearDecode();
    void resetTiming();
    void prepareForPlayback();
    void setPlaybackMode(bool on);

    QString decodedText() const { return m_scrollText; }
    QString trailingText(int maxChars) const;
    int wpm() const;
    double wpmF() const;
    double unitSeconds() const { return m_unitSeconds; }
    double ditWidthSec() const { return m_ditWidthSec; }
    double dahWidthSec() const { return m_dahWidthSec; }
    double measuredDitSec() const { return m_measDitSec; }
    double measuredDahSec() const { return m_measDahSec; }
    bool isHeadless() const { return m_headless; }

    float lastDitScore() const { return m_lastDitScore; }
    float lastDahScore() const { return m_lastDahScore; }
    float peakHoldDb() const { return m_peakHoldDb; }
    bool looking() const { return m_looking && !m_ditHit && !m_dahHit; }
    bool ditHit() const { return m_ditHit; }
    bool dahHit() const { return m_dahHit; }

signals:
    void textChanged(const QString &text);
    /**
     * Dit/dah mask geometry for the Signal Trace scope overlay.
     * Widths are independent measured/manual dit and dah durations.
     */
    void maskOverlayChanged(double ditSec, double dahSec,
                            float noiseFloorDb, float peakDb,
                            float ditScore, float dahScore,
                            bool looking, bool ditHit, bool dahHit);

private slots:
    void onClearClicked();
    void onResetTimingClicked();
    void onWpmSliderChanged(int wpm);
    void onAutoToggled(bool checked);
    void onAutoWidthToggled(bool checked);
    void onDitWidthSliderChanged(int ms);
    void onDahWidthSliderChanged(int ms);

private:
    enum class Element { None, Dit, Dah };

    void applyWpmFromSlider();
    void applyWidthSliders();
    void syncWidthSlidersFromEffective();
    void appendChar(QChar ch);
    void noteMarkDuration(double markSeconds);
    void recomputeDitDahWidths();
    void recomputeUnitFromMarks();
    /**
     * Score full key-high pulse [onsetIdx, endIdx) against dit/dah masks by
     * how completely pulse energy fills each mask (and vice versa).
     * Called on falling edge so rise + fall are both in the measurement.
     * allowLongSplit: safety-net for glued O (---) → equal dahs.
     */
    void tryMatchPulse(int onsetIdx, int endIdx, bool allowLongSplit = true);
    /**
     * While thr key stays high, detect amplitude valleys (element gaps inside
     * a glued multi-dah mark). Same OA fix as Threshold: O was one 1139 ms blob.
     */
    void processIntraMarkValley(float powerDb, qint64 nowMs);
    /** Commit pulse up to valley onset via mask fill, resume next segment. */
    void splitMarkAtValley(qint64 valleyStartMs, qint64 nowMs);
    /** Last history index with time_ms <= tMs; -1 if none. */
    int histIndexAtOrBefore(qint64 tMs) const;
    /**
     * Expand thr-high [onset,end) using envelope energy (not thr), so clipped
     * digit dahs (9=----.) still score as dah when thr only tagged the tip.
     */
    void extendPulseByEnergy(int *onsetIdx, int *endIdx) const;
    /** True when pending / last letter suggests RST-style digit run (5/9/0). */
    bool inDigitContext() const;
    void flushLetterIfNeeded(qint64 nowMs);
    void handleSpaceGap(double gapSec);
    void noteLetterGap(double gapSeconds);
    /** Min gap (s) to insert a word space — adaptive from letter gaps. */
    double wordMinSeconds() const;
    float maskHeightAt(float tNorm, float hStart, float hEnd, float riseFrac,
                       float fallFrac) const;
    /**
     * Geometric fill score: fixed-width mask starting at onset vs actual pulse
     * samples through endIdx. Pulse energy outside the mask and empty mask
     * both reduce the score.
     */
    float scorePulseFillMask(int onsetIdx, int endIdx, int maskWidthSamples,
                             float baseDb, float hPeak) const;
    float pulseBaseDb(int onsetIdx) const;
    float pulsePeakHeight(int onsetIdx, int endIdx, float baseDb) const;
    void emitElement(Element el, double durationSec, float peakDb);
    /**
     * Look ahead ~4 dah periods (toward "now") to measure local mark widths
     * and pre-set mask height from dah tops that DETECT will soon encounter.
     */
    void updateLocalMaskFromLookahead();
    void publishMaskOverlay();
    void trimScrollBuffer();
    void refreshTextView();
    void updateUi();
    double estimateDtSec() const;
    /** Effective dit/dah widths for scoring + overlay (local look-ahead when auto). */
    double effectiveDitSec() const;
    double effectiveDahSec() const;
    float effectivePeakDb() const;
    static char morseToChar(const QString &pattern);
    static double medianOf(QVector<double> v);

    bool m_headless;

    QLabel *m_statusLabel;
    QLabel *m_maskLabel;
    QLabel *m_wpmValueLabel;
    QLabel *m_ditWidthLabel;
    QLabel *m_dahWidthLabel;
    QPlainTextEdit *m_textView;
    QPushButton *m_clearButton;
    QPushButton *m_resetTimingButton;
    QCheckBox *m_autoWpmCheck;
    QCheckBox *m_autoWidthCheck;
    QSlider *m_wpmSlider;       /* value = WPM × 10 (0.1 WPM steps) */
    QSlider *m_ditWidthSlider;  /* absolute dit width in ms */
    QSlider *m_dahWidthSlider;  /* absolute dah width in ms */

    QString m_scrollText;
    QString m_targetLabel;
    QString m_pendingMorse;
    QElapsedTimer m_clock;

    float m_thresholdDb;
    float m_noiseFloorDb;
    float m_peakHoldDb;
    float m_lastDitScore;
    float m_lastDahScore;
    bool m_looking;
    bool m_ditHit;
    bool m_dahHit;
    qint64 m_hitHoldUntilMs;

    double m_unitSeconds;       /* ≈ dit width; letter/word grouping */
    double m_sliderUnitSeconds;
    double m_ditWidthSec;       /* global / slider template dit width */
    double m_dahWidthSec;       /* global / slider template dah width */
    double m_measDitSec;        /* measured from thr key-high marks (global) */
    double m_measDahSec;
    /* Look-ahead local adaptation (newer samples than DETECT station) */
    double m_localDitSec;
    double m_localDahSec;
    float m_localPeakDb;
    float m_localFloorDb;
    bool m_localReady;
    bool m_unitCalibrated;
    bool m_autoWpm;
    bool m_autoWidth;           /* true → measured widths drive templates */

    QVector<float> m_powerHist;
    QVector<qint64> m_timeHist;
    static const int kHistMax = 600;  /* enough for mid-delay + 6-dit look-ahead */

    bool m_keyHigh;
    bool m_hasState;
    qint64 m_markStartMs;
    qint64 m_lastElementEndMs;
    qint64 m_lastSampleMs;
    int m_onsetHistIdx;
    bool m_matchedThisMark;
    float m_markPeakDb;
    float m_lastPowerDb;
    float m_segPeakDb;          /* peak of current mark segment (for valley) */
    bool m_inValley;
    qint64 m_valleyStartMs;
    /** After abandoning a non-element soft stretch, wait for thr peak before valley again. */
    bool m_valleyNeedsMarkPeak;
    /**
     * After committing digit '5', keep next ~2 letters in the same word
     * (59N) unless gap is clearly word-class.
     */
    int m_digitGroupRemain;

    QVector<double> m_allMarkSecs;
    QVector<double> m_letterGaps; /* letter-class gaps for word-space adaptive cut */
    static const int kMaxMarkHistory = 48;
    static const int kMaxLetterGaps = 24;
    static const int kMaxScrollChars = 2000;
    static constexpr float kMinOverlayDit = 0.38f;  /* short pulses: lower bar */
    static constexpr float kMinOverlayDah = 0.48f;
    /** Look-ahead horizon in dah units (height pre-adjust for upcoming marks). */
    static constexpr double kLookaheadDahs = 4.0;
};

#endif // MASKMORSEWINDOW_H
