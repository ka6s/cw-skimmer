/**
 * @file decodewidget.cpp
 * @brief Stacked, scrolling copy from each signal's private decoder
 */

#include "decodewidget.h"
#include "cwcopyformat.h"
#include "../src/cw_message_validator.h"
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QDateTime>
#include <QSizePolicy>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

namespace {

bool hasDisplayableText(const QString &text)
{
    for (const QChar &ch : text) {
        if (ch.isLetterOrNumber() || ch == QChar(' ')) {
            return true;
        }
    }
    return false;
}

}  // namespace

DecodeWidget::DecodeWidget(QWidget *parent)
    : QWidget(parent)
    , m_hideSelected(false)
    , m_selectedOffsetHz(0.0f)
    , m_centerHz(0.0f)
    , m_binWidthHz(46.875f)
    , m_numBins(1024)
    , m_viewLowHz(0.0f)
    , m_viewSpanHz(0.0f)
    , m_textScrollBar(nullptr)
    , m_pinTextEnd(true)
    , m_displayThreshold(cw_message_validator_display_threshold(CW_VALIDATION_NORMAL))
    , m_charsPerLine(kDefaultChars)
    , m_multiActive(false)
{
    setMinimumWidth(280);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setStyleSheet("background-color: #101010;");
    m_textScrollBar = new QScrollBar(Qt::Horizontal, this);
    m_textScrollBar->setObjectName(QStringLiteral("decodeHistoryScroll"));
    m_textScrollBar->setStyleSheet(
        "QScrollBar:horizontal { background: #202020; height: 12px; margin: 0; }"
        "QScrollBar::handle:horizontal { background: #9a9a9a; min-width: 24px; border-radius: 4px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }");
    m_textScrollBar->setRange(0, 0);
    m_textScrollBar->setVisible(false);
    connect(m_textScrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        m_pinTextEnd = (value >= m_textScrollBar->maximum());
        update();
    });
}

void DecodeWidget::setValidationMode(const QString &mode)
{
    const QByteArray bytes = mode.toLatin1();
    const cw_validation_mode_t vmode =
        cw_message_validator_parse_mode(bytes.constData());
    m_displayThreshold = cw_message_validator_display_threshold(vmode);
    update();
}

void DecodeWidget::setCharsPerLine(int n)
{
    m_charsPerLine = std::max(4, std::min(32, n));
    update();
}

void DecodeWidget::setFrequencyScale(float centerHz, float binWidthHz, int numBins)
{
    if (numBins > 0) {
        m_numBins = numBins;
    }
    if (binWidthHz > 0.0f) {
        m_binWidthHz = binWidthHz;
    }
    m_centerHz = centerHz;
    update();
}

void DecodeWidget::setFrequencyView(float lowHz, float spanHz)
{
    if (std::fabs(lowHz - m_viewLowHz) < 0.5f && std::fabs(spanHz - m_viewSpanHz) < 0.5f) {
        return;
    }
    m_viewLowHz = lowHz;
    m_viewSpanHz = spanHz;
    update();
}

int DecodeWidget::fitCharsForWidth() const
{
    QFont textFont(QStringLiteral("Courier New"));
    textFont.setStyleHint(QFont::Monospace);
    textFont.setPointSize(11);
    const int charW = std::max(1, QFontMetrics(textFont).horizontalAdvance(QLatin1Char('M')));
    return std::max(4, (width() - 8) / charW);
}

void DecodeWidget::updateTextScrollBar()
{
    if (!m_textScrollBar) {
        return;
    }
    int longest = 0;
    for (const DecodeLine &line : m_lines) {
        longest = std::max(longest, line.text.size());
    }
    const int fit = fitCharsForWidth();
    const int maxScroll = std::max(0, longest - fit);
    const bool pin = m_pinTextEnd || m_textScrollBar->value() >= m_textScrollBar->maximum();
    m_textScrollBar->blockSignals(true);
    m_textScrollBar->setRange(0, maxScroll);
    m_textScrollBar->setPageStep(std::max(1, fit));
    m_textScrollBar->setSingleStep(1);
    if (pin) {
        m_textScrollBar->setValue(maxScroll);
        m_pinTextEnd = true;
    }
    m_textScrollBar->blockSignals(false);
    m_textScrollBar->setVisible(maxScroll > 0);
}

int DecodeWidget::findLineIndex(float freqOffsetHz) const
{
    for (int i = 0; i < m_lines.size(); ++i) {
        if (std::fabs(m_lines[i].freqOffsetHz - freqOffsetHz) <
            static_cast<float>(kChannelMatchHz)) {
            return i;
        }
    }
    return -1;
}

int DecodeWidget::offsetToY(float freqOffsetHz) const
{
    /*
     * Mirror SpectrumWidget::binIndexToY / plotRect:
     *   plotY = 20, plotHeight = height - 60
     *   bin = n/2 + offset/binWidth
     *   y increases as frequency decreases (high freq at top of plot)
     */
    const int plotY = kPlotTop;
    const int plotHeight = std::max(1, height() - kPlotTop - kPlotBottomMargin);
    if (m_viewSpanHz > 1.0f) {
        const float frac = (m_viewLowHz + m_viewSpanHz - freqOffsetHz) / m_viewSpanHz;
        return static_cast<int>(std::lround(plotY + frac * static_cast<float>(plotHeight)));
    }
    if (m_numBins <= 1 || m_binWidthHz <= 0.0f) {
        return plotY + plotHeight / 2;
    }

    int binIndex = m_numBins / 2 +
                   static_cast<int>(std::lround(freqOffsetHz / m_binWidthHz));
    binIndex = std::max(0, std::min(m_numBins - 1, binIndex));

    const float yf = plotY +
        ((m_numBins - 1 - binIndex) / static_cast<float>(m_numBins - 1)) * plotHeight;
    return static_cast<int>(std::lround(yf));
}

void DecodeWidget::appendDecode(QString decodedText, float frequencyHz,
                                float freqOffsetHz, float confidence)
{
    /* Multi-channel path owns the panel when active */
    if (m_multiActive) {
        return;
    }

    if (frequencyHz < 100.0f && std::fabs(freqOffsetHz) < 1.0f) {
        return;
    }

    QString clean;
    clean.reserve(decodedText.size());
    for (const QChar &ch : decodedText) {
        if (ch.isLetterOrNumber() || ch == QChar(' ')) {
            clean.append(ch);
        }
    }
    if (!hasDisplayableText(clean)) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int idx = findLineIndex(freqOffsetHz);

    if (idx < 0) {
        if (m_lines.size() >= kMaxLines) {
            int oldest = 0;
            for (int i = 1; i < m_lines.size(); ++i) {
                if (m_lines[i].lastUpdateMs < m_lines[oldest].lastUpdateMs) {
                    oldest = i;
                }
            }
            m_lines.removeAt(oldest);
        }
        DecodeLine line;
        line.frequencyHz = frequencyHz;
        line.freqOffsetHz = freqOffsetHz;
        line.confidence = confidence;
        line.lastUpdateMs = now;
        line.fromMulti = false;
        line.text = clean;
        m_lines.append(line);
    } else {
        DecodeLine &line = m_lines[idx];
        line.text = clean;
        line.frequencyHz = frequencyHz;
        line.freqOffsetHz = freqOffsetHz;
        line.confidence = confidence;
        line.lastUpdateMs = now;
    }

    updateTextScrollBar();
    update();
}

void DecodeWidget::setSelectedSignal(bool selected, float offsetHz)
{
    m_hideSelected = selected;
    m_selectedOffsetHz = offsetHz;
    if (m_multiActive) {
        rebuildLines();
    }
}

void DecodeWidget::setChannels(const QVector<MultiChannelDecoder::ChannelView> &channels)
{
    m_rawChannels = channels;
    m_multiActive = true;
    rebuildLines();
}

void DecodeWidget::rebuildLines()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<bool> claimed(m_lines.size(), false);

    for (const MultiChannelDecoder::ChannelView &ch : m_rawChannels) {
        if (!ch.active) {
            continue;
        }
        if (m_hideSelected
            && std::fabs(ch.freqOffsetHz - m_selectedOffsetHz) < static_cast<float>(kChannelMatchHz)) {
            continue;
        }

        int idx = -1;
        float best = static_cast<float>(kChannelMatchHz);
        for (int i = 0; i < m_lines.size(); ++i) {
            if (claimed[i]) {
                continue;
            }
            const float dist = std::fabs(m_lines[i].freqOffsetHz - ch.freqOffsetHz);
            if (dist < best) {
                best = dist;
                idx = i;
            }
        }

        if (idx >= 0) {
            DecodeLine &line = m_lines[idx];
            /* Keep the vertical slot chosen when this trace first appeared.
             * The decoder string is the buffer; the paint shows a window into it. */
            if (!ch.text.isEmpty()) {
                line.text = ch.text;
            }
            line.frequencyHz = ch.frequencyHz;
            line.confidence = std::min(1.0f, std::max(0.0f, (ch.snrDb - 3.0f) / 20.0f));
            line.lastUpdateMs = now;
            line.fromMulti = true;
            claimed[idx] = true;
            continue;
        }

        if (ch.text.isEmpty() || m_lines.size() >= kMaxLines) {
            continue;
        }
        DecodeLine line;
        line.frequencyHz = ch.frequencyHz;
        line.freqOffsetHz = ch.freqOffsetHz;
        line.confidence = std::min(1.0f, std::max(0.0f, (ch.snrDb - 3.0f) / 20.0f));
        line.lastUpdateMs = now;
        line.fromMulti = true;
        line.text = ch.text;
        m_lines.append(line);
        claimed.append(true);
    }

    /* A quiet signal stays claimed, so its copy stays on the trace. An
     * unclaimed line is one whose signal was dropped for a stronger newcomer,
     * or the trace the bottom decoder has taken. */
    for (int i = m_lines.size() - 1; i >= 0; --i) {
        if (!claimed[i]) {
            m_lines.removeAt(i);
        }
    }
    updateTextScrollBar();
    update();
}

void DecodeWidget::clear()
{
    m_lines.clear();
    m_rawChannels.clear();
    m_multiActive = false;
    updateTextScrollBar();
    update();
}

QSize DecodeWidget::sizeHint() const
{
    return QSize(340, 300);
}

void DecodeWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x10, 0x10, 0x10));

    if (m_lines.isEmpty()) {
        painter.setPen(QColor(0x88, 0x88, 0x88));
        painter.setFont(QFont(QStringLiteral("Courier New"), 11));
        painter.drawText(8, kPlotTop + 8, "Copy appears on each CW trace.");
        return;
    }

    QFont textFont(QStringLiteral("Courier New"));
    textFont.setStyleHint(QFont::Monospace);
    textFont.setPointSize(11);
    const QFontMetrics textFm(textFont);
    const int charW = std::max(1, textFm.horizontalAdvance(QLatin1Char('M')));
    const int lineHeight = textFm.height() + 2;
    const int textX = 4;
    const int fitChars = std::max(4, (width() - textX - 4) / charW);
    const int plotY = kPlotTop;
    const int plotBottom = height() - kPlotBottomMargin;
    const int fromEnd = (m_textScrollBar && m_textScrollBar->maximum() > 0)
                            ? (m_textScrollBar->maximum() - m_textScrollBar->value())
                            : 0;
    for (const DecodeLine &line : m_lines) {
        if (line.text.isEmpty()) {
            continue;
        }
        if (m_viewSpanHz > 1.0f
            && (line.freqOffsetHz < m_viewLowHz
                || line.freqOffsetHz > m_viewLowHz + m_viewSpanHz)) {
            continue;
        }
        const int yCenter = offsetToY(line.freqOffsetHz);
        if (yCenter < plotY || yCenter > plotBottom) {
            continue;
        }
        const int rowTop = yCenter - lineHeight / 2;

        /* The stored line is the buffer. This paints one window into it,
         * newest character in the rightmost column unless the bar is scrolled back. */
        const int end = line.text.size() - fromEnd;
        if (end <= 0) {
            continue;
        }
        const int start = std::max(0, end - fitChars);
        const QString body = line.text.mid(start, end - start);
        const int pad = fitChars - body.size();
        const QString field = QString(pad, QLatin1Char(' ')) + body;

        const bool highConfidence = line.confidence >= m_displayThreshold;
        const QColor base = highConfidence ? QColor(0xf2, 0xf2, 0xf2) : QColor(0x8d, 0x8d, 0x8d);
        QVector<QColor> cols(fitChars, base);
        for (const CwCopyMark &mark : cwCopyMarks(line.text)) {
            if (mark.end <= start || mark.begin >= end) {
                continue;
            }
            const int from = std::max(pad, mark.begin - start + pad);
            const int to = std::min(fitChars, mark.end - start + pad);
            for (int i = from; i < to; ++i) {
                cols[i] = mark.color;
            }
        }

        painter.setFont(textFont);
        int i = 0;
        while (i < field.size()) {
            int j = i + 1;
            while (j < field.size() && cols[j] == cols[i]) {
                ++j;
            }
            painter.setPen(cols[i]);
            const QRect run(textX + i * charW, rowTop, (j - i) * charW, lineHeight);
            painter.drawText(run, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip,
                             field.mid(i, j - i));
            i = j;
        }
    }
}

void DecodeWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_textScrollBar) {
        const int h = std::max(14, m_textScrollBar->sizeHint().height());
        m_textScrollBar->setGeometry(2, height() - h - 2, std::max(1, width() - 4), h);
    }
    updateTextScrollBar();
}

void DecodeWidget::wheelEvent(QWheelEvent *event)
{
    const int dx = event->angleDelta().x();
    const int dy = event->angleDelta().y();
    if ((event->modifiers() & Qt::ShiftModifier) && m_textScrollBar && dy != 0) {
        const int steps = (dy > 0) ? std::max(1, dy / 120) : std::min(-1, dy / 120);
        m_textScrollBar->setValue(m_textScrollBar->value() + steps);
        event->accept();
        return;
    }
    if (dx != 0 && m_textScrollBar) {
        const int steps = (dx > 0) ? std::max(1, dx / 120) : std::min(-1, dx / 120);
        m_textScrollBar->setValue(m_textScrollBar->value() + steps);
        event->accept();
        return;
    }
    if (dy != 0) {
        emit verticalScrollRequested(dy);
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}
