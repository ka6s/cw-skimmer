/**
 * @file decodewidget.h
 * @brief Per-channel CW decode display beside the waterfall
 *
 * Lines are painted at the same vertical IF position as the waterfall
 * trace they decode. Each line shows a rolling window of the latest
 * characters (default 10): new chars append on the right; oldest drop
 * from the left.
 */

#ifndef DECODEWIDGET_H
#define DECODEWIDGET_H

#include <QWidget>
#include <QString>
#include <QVector>
#include "multichanneldecoder.h"

class DecodeWidget : public QWidget {
    Q_OBJECT

public:
    explicit DecodeWidget(QWidget *parent = nullptr);

    void clear();
    void setFrequencyScale(float centerHz, float binWidthHz, int numBins);
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

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize sizeHint() const override;

private:
    struct DecodeLine {
        float frequencyHz;
        float freqOffsetHz;
        QString text;       /* already truncated to m_charsPerLine */
        float confidence;
        qint64 lastUpdateMs;
        bool fromMulti;
    };

    static QString formatFrequencyKhz(float frequencyHz);
    static QString rollingWindow(const QString &full, int maxChars);
    int findLineIndex(float freqOffsetHz) const;
    int offsetToY(float freqOffsetHz) const;

    QVector<DecodeLine> m_lines;
    float m_centerHz;
    float m_binWidthHz;
    int m_numBins;
    float m_displayThreshold;
    int m_charsPerLine;
    bool m_multiActive;  /* true after first multi-channel update */

    /* Must match SpectrumWidget::plotRect top/margins for vertical alignment */
    static const int kPlotTop = 20;
    static const int kPlotBottomMargin = 40;
    static const int kHeaderHeight = 18;
    static const int kMaxLines = 16;
    static const int kChannelMatchHz = 80;
    static const int kDefaultChars = 10;
};

#endif // DECODEWIDGET_H
