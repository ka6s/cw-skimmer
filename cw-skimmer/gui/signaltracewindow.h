/**
 * @file signaltracewindow.h
 * @brief Oscilloscope-style signal strength vs time for one selected CW tone
 */

#ifndef SIGNALTRACEWINDOW_H
#define SIGNALTRACEWINDOW_H

#include <QDialog>
#include <QVector>
#include <QString>
#include <QElapsedTimer>

class QLabel;
class QPushButton;
class QComboBox;
class QSlider;
class QCheckBox;
class QTimer;
class SignalTracePlot;

struct TraceSample {
    qint64 timeMs;
    float powerDb;
    bool aboveThreshold;
};

/**
 * Separate window showing envelope / power of a single frequency over time.
 * Opened by clicking a signal on the main waterfall display.
 *
 * Uses QWidget (not QDialog) so modal message boxes on the main window cannot
 * dismiss or hide this tool window.
 */
class SignalTraceWindow : public QWidget {
    Q_OBJECT

public:
    explicit SignalTraceWindow(QWidget *parent = nullptr);

    /** Retune the scope to a new IF offset / absolute frequency. Clears history. */
    void setTarget(float freqOffsetHz, float absFreqHz, float binWidthHz);

    /** Effective IF offset (base click + fine tune slider). */
    float targetOffsetHz() const;
    float targetAbsFreqHz() const;
    bool hasTarget() const { return m_hasTarget; }
    bool isFrozen() const { return m_frozen; }

    /** Half-width of bins averaged around the target (0 = single bin). */
    int halfBins() const;

    /** Current decode threshold in dB. */
    float thresholdDb() const { return m_thresholdDb; }

    bool afcEnabled() const;
    float afcSearchHalfHz() const;

    /**
     * Process one spectrum FFT column: optional AFC peak-lock, then measure power
     * and append to the scope / decoder.
     * @return locked IF offset after AFC (for waterfall marker)
     */
    float processSpectrumColumn(const QVector<float> &spectrum, float binWidth,
                                float noiseFloorDb);

    /**
     * Append one power sample (dB) for the current target.
     * Emits sampleReady() for the threshold Morse decoder.
     * @param forceTimeMs  if ≥ 0, use as sample timestamp (playback); else wall clock
     */
    void appendSample(float powerDb, float noiseFloorDb = -200.0f, qint64 forceTimeMs = -1);

    /** Seed the plot from historical waterfall columns (oldest → newest). */
    void seedHistory(const QVector<float> &powerDbSamples, float noiseFloorDb = -200.0f);

    void clearTrace();

    /** Lock the current trace for hand-copy (same as Freeze button). */
    void freezeForHandDecode();

    /** Show a one-line status (e.g. save path) without a modal popup. */
    void setStatusNotice(const QString &text);

    /**
     * Save captured envelope + keyed audio for offline analysis.
     * Writes:
     *   pathBase.cwtrace  — metadata + time/power/threshold stream
     *   pathBase.wav      — mono 16-bit 8 kHz tone keyed by threshold
     * @return true on success
     */
    bool saveCapture(const QString &pathBase) const;

    int captureSampleCount() const { return m_capture.size(); }

    /**
     * Play a saved capture as if it were live: feeds power samples through
     * the same appendSample → sampleReady path used by the live spectrum.
     * Accepts .cwtrace (preferred) or .wav (uses sibling .cwtrace if present,
     * otherwise reconstructs envelope from keyed tone energy).
     * @return true if load succeeded and playback started
     */
    bool startPlayback(const QString &path);
    void stopPlayback();
    bool isPlayingBack() const { return m_playbackActive; }

    /**
     * Overlay dit/dah mask boxes on the single Signal Trace scope
     * (used by Decoder: Mask). Pass enabled=false to hide.
     * Independent ditSec / dahSec widths (measured or manual).
     * Box bottom = noiseFloorDb; top = peakDb. Left-aligned at mid DETECT.
     */
    void setMaskOverlay(bool enabled, double ditSec = 0.08, double dahSec = 0.24,
                        float noiseFloorDb = -90.0f, float peakDb = -50.0f,
                        float ditScore = 0.0f, float dahScore = 0.0f,
                        bool looking = true, bool ditHit = false, bool dahHit = false);

protected:
    void closeEvent(QCloseEvent *event) override;

signals:
    void closed();
    void freezeChanged(bool frozen);
    /** Frequency fine-tune, bandwidth, or threshold changed — reseed recommended. */
    void tuningChanged();
    /**
     * One envelope sample for Morse decode.
     * @param powerDb
     * @param aboveThreshold
     * @param sampleTimeMs  monotonic ms from window open
     */
    void sampleReady(float powerDb, bool aboveThreshold, qint64 sampleTimeMs);
    void thresholdChanged(float thresholdDb);
    void openMorseRequested();
    /** Recording toggled off with samples ready — parent should write files. */
    void recordingStoppedForSave();
    /** Recording on/off changed (for external Record button on spectrum). */
    void recordingActiveChanged(bool active);
    /** AFC moved the listen offset (waterfall marker should follow). */
    void afcOffsetChanged(float offsetHz);
    /** Offline capture playback started — parent should reset Morse decoder. */
    void playbackStarted(const QString &path);
    void playbackFinished();

public slots:
    /** Start/stop scope envelope recording (Record lives on main spectrum bar). */
    void setRecordingActive(bool on);

private slots:
    void onFreezeToggled(bool checked);
    void onClearClicked();
    void onSpanChanged(int index);
    void onFreqSliderChanged(int steps);
    void onBinsSliderChanged(int halfBins);
    void onThresholdSliderChanged(int value);
    void onOpenMorseClicked();
    void onAfcToggled(bool checked);
    void onAfcSearchChanged(int value);
    void onAutoThreshToggled(bool checked);
    void onPlayCaptureClicked();
    void onStopPlaybackClicked();
    void onPlaybackTick();

private:
    void updateTitleAndLabels();
    void syncThresholdSliderRange();
    void updateAdaptiveThreshold(float powerDb, float noiseFloorDb);
    void applyFrozenUi(bool frozen);
    void applyAfcStep(float peakOffsetHz, float peakPowerDb, float snrDb);
    int maxSamplesForSpan() const;
    bool writeCwTrace(const QString &path) const;
    bool writeKeyedWav(const QString &path) const;
    bool loadCwTraceFile(const QString &path, QString *errorOut);
    bool loadKeyedWavAsEnvelope(const QString &path, QString *errorOut);
    void injectPlaybackSample(float powerDb, float noiseFloorDb, qint64 sampleTimeMs);

    SignalTracePlot *m_plot;
    QLabel *m_freqLabel;
    QLabel *m_statsLabel;
    QLabel *m_freqSliderLabel;
    QLabel *m_binsSliderLabel;
    QLabel *m_threshSliderLabel;
    QPushButton *m_freezeButton;
    QPushButton *m_clearButton;
    QPushButton *m_morseButton;
    QPushButton *m_playButton;
    QPushButton *m_stopPlayButton;
    QCheckBox *m_afcCheck;
    QCheckBox *m_autoThreshCheck;
    QComboBox *m_spanCombo;
    QSlider *m_freqSlider;       /* 50 Hz steps, ±2 kHz */
    QSlider *m_binsSlider;       /* half-bins 0..16 */
    QSlider *m_thresholdSlider;  /* dB × 10 */
    QSlider *m_afcSearchSlider;  /* search ±Hz / 50 */
    QLabel *m_afcLabel;

    bool m_hasTarget;
    bool m_frozen;
    bool m_recording;
    bool m_afcEnabled;
    bool m_afcLocked;
    bool m_keyIsHigh;       /* hysteresis state for threshold decision */
    float m_baseOffsetHz;   /* IF offset at click */
    float m_baseAbsHz;      /* absolute RF at click */
    float m_offsetHz;       /* effective listen offset (AFC or manual) */
    float m_absFreqHz;
    float m_binWidthHz;
    float m_afcLockHz;      /* smoothed AFC lock */
    float m_afcPeakPowerDb;
    float m_afcSnrDb;
    float m_lastPowerDb;
    float m_peakPowerDb;
    float m_minPowerDb;
    float m_noiseFloorDb;
    float m_thresholdDb;
    float m_trackLowDb;     /* slow low band (spaces) */
    float m_trackHighDb;    /* slow high band (marks) */
    float m_noiseEmaDb;     /* EMA of spectrum noise floor (weak prior) */
    /*
     * Auto thr scale (user model, 164845):
     *  - average noise = bottom of scale (0%)
     *  - PEAK line = absolute top of dits/dahs (100%)
     *  - Auto thr = 15% below bit tops = noise + 0.85*(bitTop-noise)
     *    so thr intersects mark peaks rather than sitting above them.
     */
    float m_noiseAvgDb;      /* average key-up / grass level */
    float m_noisePeakDb;     /* peak of grass (floor guard) */
    float m_markPeakHoldDb;  /* absolute top of bits (drawn PEAK) */
    float m_markBodyDb;      /* typical mark tops for thr placement */
    bool m_trackInit;
    int m_sampleCount;
    int m_afcHoldCount;     /* samples held during key-up */
    /* Debounce raw Schmitt with min dit time (ref WPM) to reject noise spikes */
    bool m_keyPendingHigh;
    qint64 m_keyEdgeStartMs;
    qint64 m_lastDetectTimeMs;  /* skip re-keying the same mid-scope sample */
    int m_refWpm;           /* used only for min-dit noise reject (default 20) */
    QString m_statusNotice;
    QElapsedTimer m_clock;

    /* Rolling power history for slow percentile-based auto threshold */
    QVector<float> m_powerHist;
    static const int kThreshHistMax = 280;  /* ~5–6 s — still need peak-hold for longer gaps */

    /* Full capture buffer (not limited to display span) for save/replay */
    QVector<TraceSample> m_capture;
    static const int kMaxCaptureSamples = 60000;  /* ~20 min at 50 Hz */

    /* Parallel timestamps for mid-scope delayed detection (matches plot samples) */
    QVector<qint64> m_sampleTimes;

    /* Offline playback of .cwtrace / .wav as live envelope */
    struct PlaybackSample {
        qint64 relTimeMs;   /* ms from first sample in file */
        float powerDb;
    };
    QVector<PlaybackSample> m_playbackSamples;
    QTimer *m_playbackTimer;
    bool m_playbackActive;
    bool m_playbackInjecting;  /* true while injectPlaybackSample runs */
    int m_playbackIndex;
    qint64 m_playbackWallStartMs;
    float m_playbackNoiseFloorDb;
    float m_playbackSpeed;     /* 1.0 = realtime */
    QString m_playbackPath;
};

/**
 * Plot widget: power (dB) vs time, scrolling right-to-left like a scope.
 */
class SignalTracePlot : public QWidget {
    Q_OBJECT

public:
    explicit SignalTracePlot(QWidget *parent = nullptr);

    void setMaxSamples(int n);
    void clear();
    void append(float powerDb);
    void seed(const QVector<float> &powerDbSamples);
    void setNoiseFloor(float noiseFloorDb);
    void setFrozen(bool frozen);
    void setThresholdDb(float thresholdDb);
    /** Absolute top of dits/dahs (drawn as PEAK line). */
    void setMarkPeakDb(float markPeakDb);
    void setShowThreshold(bool show);
    void setSpanSeconds(int seconds);

    /**
     * Overlay dit/dah geometric mask boxes on the scope (Decoder: Mask mode).
     * Station left-aligned at mid-scope detection cursor.
     * Independent dit/dah widths in seconds; height noiseFloorDb → peakDb.
     */
    void setMaskOverlay(bool enabled, double ditSec, double dahSec,
                        float noiseFloorDb, float peakDb,
                        float ditScore, float dahScore, bool looking,
                        bool ditHit, bool dahHit);

    float lastPower() const;
    float peakPower() const;
    float minPower() const;
    float sampleAt(int index) const;
    /**
     * Index of sample at the mid-scope DETECT cursor under fixed time scale.
     * Returns -1 until the trace has scrolled far enough to reach mid.
     */
    int midSampleIndex() const;
    /** Seconds per sample under the fixed span/maxSamples time base. */
    double samplePeriodSec() const;
    float displayMinDb() const { return m_displayMinDb; }
    float displayMaxDb() const { return m_displayMaxDb; }
    int sampleCount() const { return displaySamples().size(); }
    float thresholdDb() const { return m_thresholdDb; }
    float noiseFloorDb() const { return m_noiseFloorDb; }
    bool isFrozen() const { return m_frozen; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRect plotRect() const;
    void trimToMax();
    const QVector<float> &displaySamples() const;
    /** X pixel for sample index under fixed time scale (right = now). */
    qreal sampleToX(int index, int nSamples, const QRect &plot) const;

    QVector<float> m_samples;
    QVector<float> m_frozenSamples;  /* immutable snapshot while frozen */
    int m_maxSamples;
    int m_spanSeconds;
    float m_noiseFloorDb;
    float m_thresholdDb;
    float m_markPeakDb;     /* top of bits line (−200 = hidden) */
    bool m_showThreshold;
    bool m_frozen;
    float m_displayMinDb;
    float m_displayMaxDb;
    float m_frozenMinDb;
    float m_frozenMaxDb;

    /* Geometric mask overlay — mid-scope, left-aligned dit/dah */
    bool m_maskEnabled;
    double m_maskDitSec;    /* independent dit width (seconds) */
    double m_maskDahSec;    /* independent dah width (seconds) */
    float m_maskFloorDb;    /* noise-floor bottom of boxes */
    float m_maskPeakDb;     /* peak top of boxes */
    float m_maskDitScore;
    float m_maskDahScore;
    bool m_maskLooking;
    bool m_maskDitHit;
    bool m_maskDahHit;
};

#endif // SIGNALTRACEWINDOW_H
