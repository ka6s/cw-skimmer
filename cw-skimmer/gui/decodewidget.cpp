/**
 * @file decodewidget.cpp
 * @brief Per-channel CW decode display aligned to waterfall IF
 */

#include "decodewidget.h"
#include "../src/cw_message_validator.h"
#include <QPainter>
#include <QPaintEvent>
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
    , m_centerHz(0.0f)
    , m_binWidthHz(46.875f)
    , m_numBins(1024)
    , m_displayThreshold(cw_message_validator_display_threshold(CW_VALIDATION_NORMAL))
    , m_charsPerLine(kDefaultChars)
    , m_multiActive(false)
{
    setMinimumWidth(200);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setStyleSheet("background-color: #101010;");
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

QString DecodeWidget::formatFrequencyKhz(float frequencyHz)
{
    const qint64 roundedHz = static_cast<qint64>(std::llround(frequencyHz / 1000.0)) * 1000;
    const qint64 mhzWhole = roundedHz / 1000000;
    const qint64 khzPart = (roundedHz / 1000) % 1000;
    return QString("%1.%2")
        .arg(mhzWhole)
        .arg(khzPart, 3, 10, QChar('0'));
}

QString DecodeWidget::rollingWindow(const QString &full, int maxChars)
{
    if (maxChars <= 0 || full.isEmpty()) {
        return QString();
    }
    if (full.size() <= maxChars) {
        return full;
    }
    /* Newest characters on the right; oldest fall off the left. */
    return full.right(maxChars);
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
        line.text = rollingWindow(clean, m_charsPerLine);
        m_lines.append(line);
    } else {
        DecodeLine &line = m_lines[idx];
        /* Append only new suffix if possible */
        if (clean.startsWith(line.text) || line.text.isEmpty()) {
            line.text = rollingWindow(clean, m_charsPerLine);
        } else if (clean.length() > line.text.length()) {
            line.text = rollingWindow(clean, m_charsPerLine);
        } else {
            /* Single new letter path: treat clean as full buffer from engine */
            line.text = rollingWindow(clean, m_charsPerLine);
        }
        line.frequencyHz = frequencyHz;
        line.freqOffsetHz = freqOffsetHz;
        line.confidence = confidence;
        line.lastUpdateMs = now;
    }

    update();
}

void DecodeWidget::setChannels(const QVector<MultiChannelDecoder::ChannelView> &channels)
{
    m_multiActive = true;
    m_lines.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (const MultiChannelDecoder::ChannelView &ch : channels) {
        if (!ch.active) {
            continue;
        }
        DecodeLine line;
        line.frequencyHz = ch.frequencyHz;
        line.freqOffsetHz = ch.freqOffsetHz;
        line.confidence = std::min(1.0f, std::max(0.0f, (ch.snrDb - 3.0f) / 20.0f));
        line.lastUpdateMs = now;
        line.fromMulti = true;
        line.text = rollingWindow(ch.text, m_charsPerLine);
        m_lines.append(line);
        if (m_lines.size() >= kMaxLines) {
            break;
        }
    }
    update();
}

void DecodeWidget::clear()
{
    m_lines.clear();
    m_multiActive = false;
    update();
}

QSize DecodeWidget::sizeHint() const
{
    return QSize(220, 300);
}

void DecodeWidget::paintEvent(QPaintEvent * /*event*/)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_lines.size() - 1; i >= 0; --i) {
        if (now - m_lines[i].lastUpdateMs > 15000) {
            m_lines.removeAt(i);
        }
    }

    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x10, 0x10, 0x10));

    painter.setPen(Qt::white);
    painter.setFont(QFont("Courier", 6, QFont::Bold));
    painter.drawText(6, 12, "CW Decode");
    painter.setPen(QColor(0x66, 0x66, 0x66));
    painter.setFont(QFont("Courier", 5));
    painter.drawText(width() - 48, 12, QString("%1 ch").arg(m_charsPerLine));

    const int contentTop = kHeaderHeight;
    painter.setPen(QPen(QColor(0x44, 0x44, 0x44)));
    painter.drawLine(0, contentTop, width(), contentTop);

    if (m_lines.isEmpty()) {
        painter.setPen(QColor(0x88, 0x88, 0x88));
        painter.setFont(QFont("Courier", 6));
        painter.drawText(8, contentTop + 20, "Parallel Morse decode…");
        painter.drawText(8, contentTop + 34, "Lines track waterfall IF.");
        return;
    }

    /* ~25% larger than previous half-size fonts (6 → 8) */
    QFont textFont("Courier New", 8);
    textFont.setStyleHint(QFont::Monospace);
    QFont freqFont("Courier", 5);

    const QFontMetrics textFm(textFont);
    const int charW = std::max(1, textFm.horizontalAdvance(QLatin1String("M")));
    const int lineHeight = std::max(16, textFm.height() + 2);
    const int textX = 4;
    const int plotBottom = height() - kPlotBottomMargin;

    /* Draw each line at waterfall-aligned Y (centered on trace). */
    for (const DecodeLine &line : m_lines) {
        if (!hasDisplayableText(line.text) && line.text.isEmpty()) {
            continue;
        }

        int yCenter = offsetToY(line.freqOffsetHz);
        /* Keep fully inside content area */
        yCenter = std::max(contentTop + lineHeight / 2,
                           std::min(plotBottom - lineHeight / 2, yCenter));
        const int rowTop = yCenter - lineHeight / 2;

        /* Subtle track line at IF (matches waterfall trace height) */
        painter.setPen(QPen(QColor(0x2a, 0x3a, 0x2a), 1, Qt::DotLine));
        painter.drawLine(0, yCenter, width(), yCenter);

        painter.fillRect(0, rowTop, width(), lineHeight, QColor(0x14, 0x14, 0x18));

        const bool highConfidence = line.confidence >= m_displayThreshold;
        painter.setPen(highConfidence ? QColor(0x00, 0xff, 0x66) : QColor(0xaa, 0xaa, 0xaa));
        painter.setFont(textFont);

        /* Fixed-width rolling field: right-align newest so left chars leave first */
        const QString display = line.text;
        const int fieldW = m_charsPerLine * charW + 4;
        const QRect textRect(textX, rowTop, std::min(fieldW, width() - textX - 4), lineHeight);
        painter.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, display);

        /* Tiny freq label left of text if room, else under */
        painter.setPen(QColor(0x66, 0x99, 0xcc));
        painter.setFont(freqFont);
        const QString freqLabel = formatFrequencyKhz(line.frequencyHz);
        painter.drawText(6, rowTop - 1, freqLabel);
    }
}
