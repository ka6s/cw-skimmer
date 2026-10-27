/**
 * @file cwskmorsewindow.h
 * @brief Bottom-pane CW Skimmer channel decoder (one tuned baseband)
 *
 * Samples are the radio IQ (or real RX audio) at the trace frequency.
 * The spectrum, threshold, and mask panes do not use this window.
 */

#ifndef CWSKMORSEWINDOW_H
#define CWSKMORSEWINDOW_H

#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
struct CwskDecoder;

class CwskMorseWindow : public QWidget {
public:
    explicit CwskMorseWindow(QWidget *parent = nullptr);
    ~CwskMorseWindow() override;

    void setTargetLabel(const QString &label);
    void clearDecode();
    /** Drop the filter state. The text already on screen stays. */
    void resetTiming();
    /** Keep this copy, then append whatever the channel decodes next. */
    void adoptText(const QString &text);

    /**
     * Tune to the true tone frequency in Hz. A move of more than a few
     * hertz rebuilds the decoder and keeps the text already shown.
     * bandwidth_hz is the main filter, clamped by the decoder to 20..700.
     */
    void setTune(double sampleRate, double pitchHz, double bandwidthHz);

    /** Interleaved I,Q. complexCount is the number of pairs. */
    void feed(const float *interleavedIq, int complexCount);

    bool isTuned() const { return m_tuned; }
    QString decodedText() const;

private:
    void buildUi();
    void dropDecoder(bool keepText);
    void ensureDecoder();
    void publish();
    void refreshStatus();

    QLabel *m_statusLabel;
    QPlainTextEdit *m_textView;
    CwskDecoder *m_decoder;

    QString m_prefix;
    QString m_shown;
    QString m_target;

    bool m_tuned;
    double m_sampleRate;
    double m_pitchHz;
    double m_bandwidthHz;
};

#endif
