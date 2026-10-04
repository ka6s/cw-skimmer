/**
 * @file decodewidget.h
 * @brief CW copy for each 2 kHz spectrum slice, beside the waterfall
 *
 * Each slice that has copied text gets one line, painted on the signal that
 * slice is following. The line stays after the CW stops, including when that
 * signal is also copied in the main Morse window. The line is a window into
 * the decoder buffer: newest characters sit on the right, and the bar under
 * the copy scrolls back through text already colored. No frequency label.
 */

#ifndef DECODEWIDGET_H
#define DECODEWIDGET_H

#include <QWidget>
#include <QString>
#include <QVector>
#include "multichanneldecoder.h"

class QScrollBar;
class QResizeEvent;
class QWheelEvent;

class DecodeWidget : public QWidget {
    Q_OBJECT

public:
    explicit DecodeWidget(QWidget *parent = nullptr);

    void clear();
    void setFrequencyScale(float centerHz, float binWidthHz, int numBins);

    /** Same vertical slice the waterfall is showing. lowHz is the bottom edge. */
    void setFrequencyView(float lowHz, float spanHz);
    void setValidationMode(const QString &mode);

    /** Max characters shown per decode line (default 10). */
    void setCharsPerLine(int n);
    int charsPerLine() const { return m_charsPerLine; }

public slots:
    /**
     * Legacy single-stream append (backend ditdah path).
     * Still supported; prefers multi-channel updates when present.
     */
    void appendDecode(QString decodedText, float frequencyHz,
                      float freqOffsetHz, float confidence);

    /** Parallel multi-channel snapshot from MultiChannelDecoder. */
    void setChannels(const QVector<MultiChannelDecoder::ChannelView> &channels);

    /**
     * Remembered for callers. The side line stays when that signal is also
     * copied in the main Morse window.
     */
    void setSelectedSignal(bool selected, float offsetHz);

signals:
    /** Mouse wheel over the copy. Positive deltaY scrolls toward higher frequency. */
    void verticalScrollRequested(int deltaY);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    QSize sizeHint() const override;

private:
    struct DecodeLine {
        int slot;           /* 2 kHz slice, or -1 for the legacy single stream */
        float frequencyHz;
        float freqOffsetHz;
        QString text;       /* private decoder buffer; paint keeps what fits the row */
        float confidence;
        qint64 lastUpdateMs;
        bool fromMulti;
    };

    int findLineIndex(float freqOffsetHz) const;
    int offsetToY(float freqOffsetHz) const;
    int fitCharsForWidth() const;
    void rebuildLines();
    void updateTextScrollBar();

    QVector<DecodeLine> m_lines;
    QVector<MultiChannelDecoder::ChannelView> m_rawChannels;
    bool m_hideSelected;
    float m_selectedOffsetHz;
    float m_centerHz;
    float m_binWidthHz;
    int m_numBins;
    float m_viewLowHz;
    float m_viewSpanHz;
    QScrollBar *m_textScrollBar;
    bool m_pinTextEnd;
    float m_displayThreshold;
    int m_charsPerLine;
    bool m_multiActive;  /* true after first multi-channel update */

    /* Must match SpectrumWidget::plotRect top/margins for vertical alignment */
    static const int kPlotTop = 20;
    static const int kPlotBottomMargin = 40;
    static const int kHeaderHeight = 18;
    static const int kMaxLines = 24;
    static const int kChannelMatchHz = 2000;
    static const int kDefaultChars = 10;
};

#endif // DECODEWIDGET_H
