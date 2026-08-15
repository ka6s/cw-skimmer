/**
 * @file deepcwmorsewindow.cpp
 * @brief DeepCW ONNX Morse decode from scope envelope (OOK reconstruction)
 */

#include "deepcwmorsewindow.h"

extern "C" {
#include "deepcw_engine.h"
#include "logger.h"
}

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>

const float DeepCwMorseWindow::kToneHz = 800.0f;

DeepCwMorseWindow::DeepCwMorseWindow(QWidget *parent)
    : QWidget(parent)
    , m_statusLabel(nullptr)
    , m_textView(nullptr)
    , m_clearButton(nullptr)
    , m_resetButton(nullptr)
    , m_engine(nullptr)
    , m_ready(false)
    , m_globalInited(false)
    , m_hasState(false)
    , m_isHigh(false)
    , m_lastSampleMs(0)
    , m_lastPowerDb(-200.0f)
    , m_sampleCount(0)
    , m_playbackMode(false)
    , m_phase(0.0)
{
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(
        "QLabel { color: #222; background: #f0f0f0; border: 1px solid #888; "
        "border-radius: 4px; padding: 6px; font-size: 12px; }");

    m_textView = new QPlainTextEdit(this);
    m_textView->setReadOnly(true);
    m_textView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_textView->setPlaceholderText(
        "DeepCW neural decode appears here.\n"
        "Click a tone on the waterfall (opens Signal Trace).\n"
        "Envelope samples are turned into a keyed 800 Hz tone for the model.");
    m_textView->setStyleSheet(
        "QPlainTextEdit { background: #0a0a0c; color: #66ccff; font-family: Courier; "
        "font-size: 18px; }");

    m_clearButton = new QPushButton("Clear Text", this);
    m_resetButton = new QPushButton("Reset Engine", this);

    auto *row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(m_resetButton);
    row->addWidget(m_clearButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_textView, 1);
    layout->addLayout(row);
    setLayout(layout);

    connect(m_clearButton, &QPushButton::clicked, this, &DeepCwMorseWindow::onClearClicked);
    connect(m_resetButton, &QPushButton::clicked, this, &DeepCwMorseWindow::onResetClicked);

    m_clock.start();
    m_statusMsg = QStringLiteral("DeepCW not loaded yet");
    updateUi();
}

DeepCwMorseWindow::~DeepCwMorseWindow()
{
    if (m_engine) {
        deepcw_engine_destroy(m_engine);
        m_engine = nullptr;
    }
    /* Leave global ONNX session alive for the process lifetime. */
}

QString DeepCwMorseWindow::resolveModelPath()
{
    const QStringList candidates = {
        QStringLiteral("models/model.onnx"),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models/model.onnx")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../models/model.onnx")),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../models/model.onnx")),
    };
    for (const QString &p : candidates) {
        if (QFileInfo::exists(p)) {
            return QFileInfo(p).absoluteFilePath();
        }
    }
    return QString();
}

bool DeepCwMorseWindow::ensureReady(QString *errorOut)
{
    if (m_ready && m_engine) {
        return true;
    }

    const QString modelPath = resolveModelPath();
    if (modelPath.isEmpty()) {
        const QString err = QStringLiteral(
            "DeepCW model not found (models/model.onnx). Place the ONNX file under models/.");
        m_statusMsg = err;
        updateUi();
        if (errorOut) {
            *errorOut = err;
        }
        return false;
    }

    if (!m_globalInited) {
        if (deepcw_engine_global_init(modelPath.toUtf8().constData()) != 0) {
            const QString err = QStringLiteral("Failed to load DeepCW ONNX model:\n%1").arg(modelPath);
            m_statusMsg = err;
            updateUi();
            if (errorOut) {
                *errorOut = err;
            }
            return false;
        }
        m_globalInited = true;
    }

    if (!m_engine) {
        m_engine = deepcw_engine_create();
        if (!m_engine) {
            const QString err = QStringLiteral("deepcw_engine_create failed");
            m_statusMsg = err;
            updateUi();
            if (errorOut) {
                *errorOut = err;
            }
            return false;
        }
        deepcw_engine_set_tone_hz(m_engine, kToneHz);
    }

    m_ready = true;
    m_statusMsg = QStringLiteral("DeepCW ready — model %1").arg(QFileInfo(modelPath).fileName());
    updateUi();
    return true;
}

void DeepCwMorseWindow::setTargetLabel(const QString &label)
{
    m_targetLabel = label;
    updateUi();
}

void DeepCwMorseWindow::synthesizeAndFeed(bool keyDown, double durationSec)
{
    if (!m_engine || !m_ready || durationSec <= 0.0) {
        return;
    }

    /* Cap pathological gaps (freeze, long pause) so we do not allocate huge buffers */
    if (durationSec > 6.0) {
        durationSec = 6.0;
    }
    if (durationSec < 1.0 / kSynthRate) {
        return;
    }

    int n = static_cast<int>(std::lround(durationSec * kSynthRate));
    if (n < 1) {
        return;
    }
    if (n > kMaxChunkSamples) {
        n = kMaxChunkSamples;
    }

    QVector<float> re(n);
    QVector<float> im(n);
    const double twoPi = 2.0 * M_PI;
    const double dphi = twoPi * static_cast<double>(kToneHz) / static_cast<double>(kSynthRate);

    for (int i = 0; i < n; ++i) {
        if (keyDown) {
            re[i] = static_cast<float>(0.5 * std::sin(m_phase));
            m_phase += dphi;
            if (m_phase > twoPi) {
                m_phase -= twoPi;
            }
        } else {
            re[i] = 0.0f;
        }
        im[i] = 0.0f;
    }

    deepcw_engine_feed_iq(m_engine, re.constData(), im.constData(), n, kSynthRate);
    pullDecodedText();
}

void DeepCwMorseWindow::pullDecodedText()
{
    if (!m_engine) {
        return;
    }

    if (deepcw_engine_is_dirty(m_engine)) {
        const char *plain = deepcw_engine_get_plain_text(m_engine);
        if (plain) {
            const QString p = QString::fromUtf8(plain);
            if (p.length() > m_lastPlain.length() && p.startsWith(m_lastPlain)) {
                m_scrollText.append(p.mid(m_lastPlain.length()));
            } else if (p != m_lastPlain && !p.isEmpty()) {
                /* Model rewrote prefix — append only new tail if any */
                if (p.length() > m_lastPlain.length()) {
                    m_scrollText.append(p.mid(m_lastPlain.length()));
                } else if (!m_scrollText.endsWith(p) && p.length() > 0) {
                    /* Shorter rewrite: keep scroll, append space+new if useful */
                    if (!m_scrollText.isEmpty() && !m_scrollText.endsWith(QChar(' '))) {
                        m_scrollText.append(QChar(' '));
                    }
                    m_scrollText.append(p);
                }
            }
            m_lastPlain = p;
            trimScrollBuffer();
            refreshTextView();
        }
        deepcw_engine_clear_dirty(m_engine);
    }

    char ch = 0;
    while (deepcw_engine_take_new_letter(m_engine, &ch)) {
        /* plain_text path already updated scroll; letter events are optional */
        (void)ch;
    }
}

void DeepCwMorseWindow::feedSample(float powerDb, bool aboveThreshold, qint64 sampleTimeMs)
{
    m_lastPowerDb = powerDb;
    m_sampleCount++;

    if (!m_ready || !m_engine) {
        return;
    }

    const qint64 nowMs = (sampleTimeMs >= 0) ? sampleTimeMs : m_clock.elapsed();

    if (!m_hasState) {
        m_hasState = true;
        m_isHigh = aboveThreshold;
        m_lastSampleMs = nowMs;
        updateUi();
        return;
    }

    const double durationSec = std::max(0.0, (nowMs - m_lastSampleMs) / 1000.0);
    /* Audio for the interval that just ended used previous key state */
    synthesizeAndFeed(m_isHigh, durationSec);

    m_isHigh = aboveThreshold;
    m_lastSampleMs = nowMs;
    updateUi();
}

void DeepCwMorseWindow::clearDecode()
{
    m_scrollText.clear();
    m_lastPlain.clear();
    if (m_engine) {
        deepcw_engine_reset(m_engine);
        deepcw_engine_set_tone_hz(m_engine, kToneHz);
    }
    m_phase = 0.0;
    refreshTextView();
    updateUi();
}

void DeepCwMorseWindow::resetEngine()
{
    m_hasState = false;
    m_isHigh = false;
    m_lastSampleMs = 0;
    m_sampleCount = 0;
    m_phase = 0.0;
    m_lastPlain.clear();
    if (m_engine) {
        deepcw_engine_reset(m_engine);
        deepcw_engine_set_tone_hz(m_engine, kToneHz);
    }
    updateUi();
}

void DeepCwMorseWindow::prepareForPlayback()
{
    m_playbackMode = true;
    clearDecode();
    resetEngine();
    m_clock.restart();
}

void DeepCwMorseWindow::setPlaybackMode(bool on)
{
    m_playbackMode = on;
    if (!on && m_engine) {
        deepcw_engine_flush(m_engine);
        pullDecodedText();
    }
    updateUi();
}

void DeepCwMorseWindow::onClearClicked()
{
    clearDecode();
}

void DeepCwMorseWindow::onResetClicked()
{
    resetEngine();
    clearDecode();
}

void DeepCwMorseWindow::trimScrollBuffer()
{
    if (m_scrollText.size() > kMaxScrollChars) {
        m_scrollText = m_scrollText.right(kMaxScrollChars * 3 / 4);
    }
}

void DeepCwMorseWindow::refreshTextView()
{
    if (!m_textView) {
        return;
    }
    m_textView->setPlainText(m_scrollText);
    QTextCursor c = m_textView->textCursor();
    c.movePosition(QTextCursor::End);
    m_textView->setTextCursor(c);
}

void DeepCwMorseWindow::updateUi()
{
    if (!m_statusLabel) {
        return;
    }
    QString line = m_statusMsg;
    if (!m_targetLabel.isEmpty()) {
        line += QStringLiteral("\nTarget: %1").arg(m_targetLabel);
    }
    line += QStringLiteral("\nSamples: %1  Key: %2  Power: %3 dB%4")
                .arg(m_sampleCount)
                .arg(m_isHigh ? QStringLiteral("MARK") : QStringLiteral("SPACE"))
                .arg(m_lastPowerDb, 0, 'f', 1)
                .arg(m_playbackMode ? QStringLiteral("  [PLAYBACK]") : QString());
    if (m_ready && m_engine) {
        const char *pending = deepcw_engine_get_pending(m_engine);
        if (pending && pending[0]) {
            line += QStringLiteral("\nPending: %1").arg(QString::fromUtf8(pending));
        }
    }
    m_statusLabel->setText(line);
}
