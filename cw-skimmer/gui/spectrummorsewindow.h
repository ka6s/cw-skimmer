/**
 * @file spectrummorsewindow.h
 * @brief CW decode from the waterfall picture (white = on, black = off)
 *
 * The decision is held until six dahs of newer spectrum have been drawn,
 * so the character is complete on the scope before it is copied.
 */

#ifndef SPECTRUMMORSEWINDOW_H
#define SPECTRUMMORSEWINDOW_H

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPlainTextEdit;

class SpectrumMorseWindow : public QWidget {
public:
    explicit SpectrumMorseWindow(QWidget *parent = nullptr, bool headless = false);

    void setTargetLabel(const QString &label);
    void clearDecode();
    void resetTiming();
    /** Keep this copy, then append whatever the waterfall decodes next. */
    void adoptText(const QString &text);

    /**
     * One waterfall column at the tuned frequency.
     * ON/OFF uses the same black-to-white map as the spectrum display.
     */
    void feedColumn(float powerDb, float noiseFloorDb, float binWidthHz);

    QString decodedText() const { return m_text; }
    QString trailingText(int maxChars) const;

private:
    struct Column {
        double tSec;
        bool on;
    };
    struct Run {
        bool on;
        double sec;
    };
    struct Timing {
        double dit;
        double dahCut;
        double letterCut;
        double wordCut;
        bool ok;
    };

    void buildUi();
    void pushRun(bool on, double seconds);
    void sealOldest();
    void publish(double cursorSec);
    Timing measure(const QVector<Run> &runs) const;
    QString decodeRuns(const QVector<Run> &runs, const Timing &timing) const;
    void setText(const QString &text);
    void refreshStatus();
    double lagSec() const;

    bool m_headless;
    QLabel *m_statusLabel;
    QPlainTextEdit *m_textView;

    QString m_text;
    QString m_sealed;
    QString m_target;

    QVector<Column> m_hist;
    int m_next;
    QVector<Run> m_done;

    double m_nowSec;
    bool m_haveState;
    bool m_isOn;
    bool m_sawMark;
    double m_stateStartSec;

    double m_unitSeconds;
    bool m_unitCalibrated;
    int m_columns;
    double m_columnSec;
    /* Key stays down through a shallow dip. A real space falls to the noise. */
    bool m_latchedOn;
};

#endif
