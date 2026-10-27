/**
 * @file deepcwmorsewindow.cpp
 * @brief DeepCW on the signal selected in the waterfall
 */

#include "deepcwmorsewindow.h"

#include "cwcopyformat.h"
#include "deepcw_engine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

#include <cmath>

DeepcwMorseWindow::DeepcwMorseWindow(QWidget *parent)
    : QWidget(parent)
    , m_statusLabel(nullptr)
    , m_textView(nullptr)
    , m_front(nullptr)
    , m_tuned(false)
    , m_realAudio(false)
    , m_sampleRate(0.0)
    , m_pitchHz(0.0)
    , m_targetRate(0.0)
    , m_centerHz(800.0)
    , m_stop(false)
    , m_reset(false)
    , m_started(false)
    , m_epoch(1)
{
    buildUi();
    connect(this, &DeepcwMorseWindow::engineText, this,
            [this](quint64 epoch, const QString &text) {
                if (epoch != m_epoch.load()) {
                    return;
                }
                m_engineText = text;
                publish();
            });
    connect(this, &DeepcwMorseWindow::engineNote, this, [this](const QString &note) {
        m_note = note;
        refreshStatus();
    });
}

DeepcwMorseWindow::~DeepcwMorseWindow()
{
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    QCoreApplication::removePostedEvents(this);
    deepcw_front_destroy(m_front);
    m_front = nullptr;
}

void DeepcwMorseWindow::buildUi()
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

bool DeepcwMorseWindow::locateModel()
{
    if (!m_modelPath.isEmpty()) {
        return m_targetRate > 0.0;
    }
    QStringList dirs;
    dirs << QDir::currentPath();
    QDir app(QCoreApplication::applicationDirPath());
    dirs << app.absolutePath();
    if (app.dirName() == QStringLiteral("bin") && app.cdUp()) {
        dirs << app.absolutePath();
    }
    for (const QString &dir : dirs) {
        const QString model = QDir(dir).filePath(QStringLiteral("models/model.onnx"));
        const QString meta = model + QStringLiteral(".json");
        if (QFileInfo::exists(model) && QFileInfo::exists(meta)) {
            m_modelPath = model;
            m_metadataPath = meta;
            break;
        }
    }
    if (m_modelPath.isEmpty()) {
        m_note = QStringLiteral("DeepCW model not found (models/model.onnx)");
        refreshStatus();
        return false;
    }
    double rate = 0.0;
    double center = 0.0;
    std::string error;
    if (!deepcw_metadata_audio(m_metadataPath.toUtf8().constData(), &rate, &center, &error)) {
        m_note = QString::fromStdString(error);
        m_modelPath.clear();
        refreshStatus();
        return false;
    }
    m_targetRate = rate;
    m_centerHz = center;
    return true;
}

void DeepcwMorseWindow::engineMain(std::string modelPath, std::string metadataPath)
{
    std::string error;
    DeepCwDecoder *decoder =
        deepcw_decoder_create(modelPath.c_str(), metadataPath.c_str(), &error);
    if (!decoder) {
        emit engineNote(QString::fromStdString(error));
        return;
    }
    emit engineNote(QString());

    while (true) {
        std::vector<float> chunk;
        bool stop = false;
        bool reset = false;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [this]() { return m_stop || m_reset || !m_queue.empty(); });
            stop = m_stop;
            reset = m_reset;
            m_reset = false;
            chunk.swap(m_queue);
        }
        const quint64 epoch = m_epoch.load();
        if (stop) {
            break;
        }
        if (reset) {
            deepcw_decoder_reset(decoder);
            emit engineText(epoch, QString());
        }
        if (!chunk.empty()) {
            std::string piece;
            deepcw_decoder_process(decoder, chunk.data(), static_cast<int>(chunk.size()), &piece);
            if (!piece.empty()) {
                emit engineText(epoch, QString::fromLatin1(deepcw_decoder_text(decoder).c_str()));
            }
            if (!deepcw_decoder_error(decoder).empty()) {
                emit engineNote(QString::fromStdString(deepcw_decoder_error(decoder)));
            }
        }
    }
    deepcw_decoder_destroy(decoder);
}

void DeepcwMorseWindow::warm()
{
    if (m_started || !locateModel()) {
        return;
    }
    m_started = true;
    m_note = QStringLiteral("loading model");
    refreshStatus();
    const std::string model = m_modelPath.toStdString();
    const std::string meta = m_metadataPath.toStdString();
    m_thread = std::thread([this, model, meta]() { engineMain(model, meta); });
}

void DeepcwMorseWindow::ensureFront()
{
    if (m_front || !(m_targetRate > 0.0) || !(m_sampleRate > 0.0)) {
        return;
    }
    m_front = deepcw_front_create(m_sampleRate, m_pitchHz, m_realAudio ? 1 : 0, m_targetRate,
                                  m_centerHz);
}

void DeepcwMorseWindow::bumpEpoch()
{
    m_epoch.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_queue.clear();
        m_reset = true;
    }
    m_cv.notify_all();
}

void DeepcwMorseWindow::setTargetLabel(const QString &label)
{
    m_target = label;
    refreshStatus();
}

void DeepcwMorseWindow::clearDecode()
{
    m_prefix.clear();
    m_engineText.clear();
    publish();
    if (m_front) {
        deepcw_front_reset(m_front, m_sampleRate, m_pitchHz, m_realAudio ? 1 : 0);
    }
    bumpEpoch();
    refreshStatus();
}

void DeepcwMorseWindow::resetTiming()
{
    m_prefix = m_shown;
    m_engineText.clear();
    publish();
    if (m_front) {
        deepcw_front_reset(m_front, m_sampleRate, m_pitchHz, m_realAudio ? 1 : 0);
    }
    bumpEpoch();
    refreshStatus();
}

void DeepcwMorseWindow::adoptText(const QString &text)
{
    m_prefix = text;
    m_engineText.clear();
    publish();
    if (m_front) {
        deepcw_front_reset(m_front, m_sampleRate, m_pitchHz, m_realAudio ? 1 : 0);
    }
    bumpEpoch();
    refreshStatus();
}

void DeepcwMorseWindow::setTune(double sampleRate, double pitchHz, bool realAudio)
{
    warm();
    if (!(sampleRate >= 100.0) || !std::isfinite(pitchHz)) {
        return;
    }
    const bool changed = !m_tuned || std::fabs(sampleRate - m_sampleRate) > 0.5 ||
                         std::fabs(pitchHz - m_pitchHz) > 8.0 || realAudio != m_realAudio;
    m_sampleRate = sampleRate;
    m_pitchHz = pitchHz;
    m_realAudio = realAudio;
    m_tuned = true;
    ensureFront();
    if (changed && m_front) {
        deepcw_front_reset(m_front, m_sampleRate, m_pitchHz, m_realAudio ? 1 : 0);
        m_prefix = m_shown;
        m_engineText.clear();
        publish();
        bumpEpoch();
    }
    refreshStatus();
}

void DeepcwMorseWindow::feed(const float *interleavedIq, int complexCount)
{
    if (!interleavedIq || complexCount <= 0 || !m_tuned) {
        return;
    }
    ensureFront();
    if (!m_front) {
        return;
    }
    std::vector<float> pcm;
    deepcw_front_process(m_front, interleavedIq, complexCount, &pcm);
    if (pcm.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_queue.insert(m_queue.end(), pcm.begin(), pcm.end());
    }
    m_cv.notify_all();
}

void DeepcwMorseWindow::publish()
{
    QString shown = m_prefix + m_engineText;
    if (shown.size() > 8000) {
        shown = shown.right(6000);
        m_prefix = shown;
        m_engineText.clear();
    }
    if (shown == m_shown) {
        return;
    }
    const bool follow = !m_textView || m_textView->textCursor().atEnd() ||
                        m_textView->toPlainText().isEmpty();
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

void DeepcwMorseWindow::refreshStatus()
{
    if (!m_statusLabel) {
        return;
    }
    QString line;
    if (!m_note.isEmpty()) {
        line = QStringLiteral("DeepCW  ·  %1").arg(m_note);
    } else if (!m_tuned) {
        line = QStringLiteral("DeepCW  ·  click a signal on the waterfall");
    } else {
        line = QStringLiteral("DeepCW  ·  %1 Hz audio  ·  trace %2 Hz")
                   .arg(m_centerHz, 0, 'f', 0)
                   .arg(m_pitchHz, 0, 'f', 0);
    }
    if (!m_target.isEmpty()) {
        line += QStringLiteral("  ·  %1").arg(m_target);
    }
    m_statusLabel->setText(line);
}
