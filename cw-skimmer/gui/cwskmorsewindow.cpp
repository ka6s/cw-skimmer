/**
 * @file cwskmorsewindow.cpp
 * @brief One VE3NEA CW Skimmer channel on the selected trace
 */

#include "cwskmorsewindow.h"
#include "cwcopyformat.h"
#include "cwsk_decoder.h"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

#include <cmath>

CwskMorseWindow::CwskMorseWindow(QWidget *parent)
    : QWidget(parent)
    , m_statusLabel(nullptr)
    , m_textView(nullptr)
    , m_decoder(nullptr)
    , m_tuned(false)
    , m_sampleRate(0.0)
    , m_pitchHz(0.0)
    , m_bandwidthHz(150.0)
{
    buildUi();
}

CwskMorseWindow::~CwskMorseWindow()
{
    cwsk_decoder_destroy(m_decoder);
    m_decoder = nullptr;
}

void CwskMorseWindow::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(
        "color: #d0d0d0; font-family: 'JetBrains Mono', monospace; font-size: 11px;");
    layout->addWidget(m_statusLabel);

    m_textView = new QPlainTextEdit(this);
    m_textView->setReadOnly(true);
    m_textView->setStyleSheet(
        "QPlainTextEdit { background: #101010; color: #f2f2f2; "
        "font-family: 'JetBrains Mono', monospace; font-size: 16px; }");
    installCwCopyHighlighter(m_textView);
    layout->addWidget(m_textView, 1);

    auto *clearButton = new QPushButton(QStringLiteral("Clear"), this);
    layout->addWidget(clearButton);
    connect(clearButton, &QPushButton::clicked, this, [this]() { clearDecode(); });

    refreshStatus();
}

void CwskMorseWindow::setTargetLabel(const QString &label)
{
    m_target = label;
    refreshStatus();
}

void CwskMorseWindow::dropDecoder(bool keepText)
{
    if (m_decoder) {
        if (keepText) {
            const char *got = cwsk_decoder_text(m_decoder);
            if (got && got[0]) {
                m_prefix += QString::fromLatin1(got);
            }
        }
        cwsk_decoder_destroy(m_decoder);
        m_decoder = nullptr;
    }
    if (!keepText) {
        m_prefix.clear();
    }
}

void CwskMorseWindow::clearDecode()
{
    dropDecoder(false);
    m_shown.clear();
    if (m_textView) {
        m_textView->clear();
    }
    refreshStatus();
}

void CwskMorseWindow::resetTiming()
{
    dropDecoder(true);
    publish();
    refreshStatus();
}

void CwskMorseWindow::adoptText(const QString &text)
{
    dropDecoder(false);
    m_prefix = text;
    publish();
    refreshStatus();
}

void CwskMorseWindow::setTune(double sampleRate, double pitchHz, double bandwidthHz)
{
    if (!(sampleRate >= 100.0) || !std::isfinite(pitchHz) || !std::isfinite(bandwidthHz)) {
        return;
    }
    const bool changed = !m_tuned
        || std::fabs(sampleRate - m_sampleRate) > 0.5
        || std::fabs(pitchHz - m_pitchHz) > 8.0
        || std::fabs(bandwidthHz - m_bandwidthHz) > 10.0;
    m_sampleRate = sampleRate;
    m_pitchHz = pitchHz;
    m_bandwidthHz = bandwidthHz;
    m_tuned = true;
    if (changed) {
        dropDecoder(true);
        refreshStatus();
    }
}

void CwskMorseWindow::ensureDecoder()
{
    if (m_decoder || !m_tuned || m_sampleRate < 100.0) {
        return;
    }
    m_decoder = cwsk_decoder_create(m_sampleRate, m_pitchHz, m_bandwidthHz, 1);
}

void CwskMorseWindow::feed(const float *interleavedIq, int complexCount)
{
    if (!interleavedIq || complexCount <= 0 || !m_tuned) {
        return;
    }
    ensureDecoder();
    if (!m_decoder) {
        return;
    }
    char scratch[256];
    (void)cwsk_decoder_process(m_decoder, interleavedIq, complexCount * 2, scratch,
                               static_cast<int>(sizeof scratch));
    publish();
}

QString CwskMorseWindow::decodedText() const
{
    return m_shown;
}

void CwskMorseWindow::publish()
{
    QString shown = m_prefix;
    if (m_decoder) {
        const char *got = cwsk_decoder_text(m_decoder);
        if (got && got[0]) {
            shown += QString::fromLatin1(got);
        }
    }
    if (shown.size() > 8000) {
        shown = shown.right(6000);
        if (m_decoder) {
            cwsk_decoder_destroy(m_decoder);
            m_decoder = nullptr;
        }
        m_prefix = shown;
    }
    if (shown == m_shown) {
        return;
    }
    const bool follow = !m_textView || m_textView->textCursor().atEnd()
        || m_textView->toPlainText().isEmpty();
    m_shown = shown;
    if (m_textView) {
        m_textView->setPlainText(m_shown);
        applyCwCopyColors(m_textView);
        if (follow) {
            QTextCursor cursor = m_textView->textCursor();
            cursor.movePosition(QTextCursor::End);
            m_textView->setTextCursor(cursor);
            m_textView->ensureCursorVisible();
        }
    }
    refreshStatus();
}

void CwskMorseWindow::refreshStatus()
{
    if (!m_statusLabel) {
        return;
    }
    QString line;
    if (!m_tuned) {
        line = QStringLiteral("Cwsk  ·  click a signal on the waterfall");
    } else {
        const double wpm = m_decoder ? cwsk_decoder_wpm(m_decoder) : 0.0;
        line = QStringLiteral("Cwsk  ·  %1 Hz  ·  filter %2 Hz  ·  %3 WPM")
                   .arg(m_pitchHz, 0, 'f', 0)
                   .arg(m_bandwidthHz, 0, 'f', 0)
                   .arg(wpm, 0, 'f', 0);
    }
    if (!m_target.isEmpty()) {
        line += QStringLiteral("  ·  %1").arg(m_target);
    }
    m_statusLabel->setText(line);
}
