#include "cwkeyboard.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QClipboard>

CwKeyboard::CwKeyboard(QWidget *parent)
    : QWidget(parent)
    , m_sent(nullptr)
    , m_status(nullptr)
    , m_input(nullptr)
    , m_wpm(nullptr)
    , m_stop(nullptr)
    , m_flush(nullptr)
    , m_armed(false)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_sent = new QLabel(QStringLiteral("Sent"), this);
    m_sent->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sent->setStyleSheet(QStringLiteral("QLabel { font-family: \"Courier New\", monospace; }"));

    auto *row = new QHBoxLayout();
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("Type CW here"));
    m_input->setToolTip(QStringLiteral(
        "Characters send through the radio.\n"
        "Enter adds a word space and sends now.\n"
        "Backspace removes a character that has not been sent.\n"
        "Esc stops the radio and clears the unsent text.\n"
        "The radio should already be in CW."));
    m_input->installEventFilter(this);
    m_input->setClearButtonEnabled(false);

    m_wpm = new QSpinBox(this);
    m_wpm->setRange(5, 60);
    m_wpm->setValue(20);
    m_wpm->setSuffix(QStringLiteral(" WPM"));
    m_wpm->setToolTip(QStringLiteral("Keyer speed on the radio"));

    m_stop = new QPushButton(QStringLiteral("Stop"), this);
    m_stop->setToolTip(QStringLiteral("Stop sending and drop text that has not gone out"));

    row->addWidget(m_input, 1);
    row->addWidget(m_wpm, 0);
    row->addWidget(m_stop, 0);

    m_status = new QLabel(QStringLiteral("Start the radio to send CW"), this);

    layout->addWidget(m_sent);
    layout->addLayout(row);
    layout->addWidget(m_status);

    m_flush = new QTimer(this);
    m_flush->setSingleShot(true);
    m_flush->setInterval(120);
    connect(m_flush, &QTimer::timeout, this, &CwKeyboard::flushPending);
    connect(m_stop, &QPushButton::clicked, this, [this]() {
        m_pending.clear();
        showPending();
        m_flush->stop();
        emit stopRequested();
    });
    connect(m_wpm, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int wpm) {
        emit wpmChanged(wpm);
    });
}

void CwKeyboard::setWpm(int wpm)
{
    if (!m_wpm) {
        return;
    }
    m_wpm->blockSignals(true);
    m_wpm->setValue(wpm);
    m_wpm->blockSignals(false);
}

int CwKeyboard::wpm() const
{
    return m_wpm ? m_wpm->value() : 20;
}

void CwKeyboard::setTransportLabel(const QString &label)
{
    if (m_status && m_armed) {
        m_status->setText(label);
    }
    m_status->setToolTip(label);
    setProperty("transportLabel", label);
}

void CwKeyboard::setArmed(bool armed, const QString &reason)
{
    m_armed = armed;
    m_reason = reason;
    if (!armed) {
        m_flush->stop();
        if (m_status) {
            m_status->setText(reason);
        }
        return;
    }
    const QString transport = property("transportLabel").toString();
    if (m_status) {
        m_status->setText(transport.isEmpty() ? reason : transport);
    }
    if (!m_pending.isEmpty()) {
        m_flush->start();
    }
}

void CwKeyboard::sendImmediate(const QString &text)
{
    QString clean;
    clean.reserve(text.size());
    bool any = false;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QLatin1Char('\t')) {
            if (!clean.endsWith(QLatin1Char(' '))) {
                clean.append(QLatin1Char(' '));
            }
            continue;
        }
        if (ch.unicode() < 32) {
            continue;
        }
        if (!ch.isSpace()) {
            any = true;
        }
        clean.append(ch);
    }
    if (!any) {
        return;
    }
    if (!m_armed) {
        if (m_status) {
            m_status->setText(m_reason.isEmpty()
                                  ? QStringLiteral("Start the radio to send CW")
                                  : m_reason);
        }
        return;
    }
    rememberSent(clean);
    emit textReady(clean);
}

void CwKeyboard::appendText(const QString &text)
{
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QLatin1Char('\t')) {
            if (!m_pending.endsWith(QLatin1Char(' '))) {
                m_pending.append(QLatin1Char(' '));
            }
            continue;
        }
        if (ch.unicode() < 32) {
            continue;
        }
        m_pending.append(ch);
    }
    if (m_pending.size() > 180) {
        m_pending.truncate(180);
    }
    showPending();
    if (m_pending.size() >= 24) {
        flushPending();
        return;
    }
    if (m_armed) {
        m_flush->start();
    }
}

void CwKeyboard::showPending()
{
    if (!m_input) {
        return;
    }
    m_input->setText(m_pending);
    m_input->setCursorPosition(m_pending.size());
}

void CwKeyboard::rememberSent(const QString &text)
{
    if (!m_sent) {
        return;
    }
    QString shown = m_sent->text();
    if (shown == QLatin1String("Sent")) {
        shown.clear();
    }
    shown.append(text);
    if (shown.size() > 120) {
        shown = shown.right(120);
    }
    m_sent->setText(shown);
}

void CwKeyboard::flushPending()
{
    if (!m_armed || m_pending.isEmpty()) {
        return;
    }
    const QString text = m_pending;
    m_pending.clear();
    showPending();
    rememberSent(text);
    emit textReady(text);
}

bool CwKeyboard::eventFilter(QObject *obj, QEvent *event)
{
    if (obj != m_input || event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(obj, event);
    }

    auto *key = static_cast<QKeyEvent *>(event);
    if (key->matches(QKeySequence::Paste)) {
        const QString clip = QGuiApplication::clipboard()
                                 ? QGuiApplication::clipboard()->text()
                                 : QString();
        appendText(clip);
        return true;
    }
    if (key->key() == Qt::Key_Escape) {
        m_pending.clear();
        showPending();
        m_flush->stop();
        emit stopRequested();
        return true;
    }
    if (key->key() == Qt::Key_Backspace) {
        if (!m_pending.isEmpty()) {
            m_pending.chop(1);
            showPending();
        }
        return true;
    }
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        if (!m_pending.endsWith(QLatin1Char(' '))) {
            m_pending.append(QLatin1Char(' '));
        }
        showPending();
        flushPending();
        if (!m_armed && m_status) {
            m_status->setText(m_reason.isEmpty()
                                  ? QStringLiteral("Start the radio to send CW")
                                  : m_reason);
        }
        return true;
    }
    if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
        return false;
    }
    const QString text = key->text();
    if (text.isEmpty() || key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        return false;
    }
    appendText(text);
    if (!m_armed && m_status) {
        m_status->setText(m_reason.isEmpty()
                              ? QStringLiteral("Start the radio to send CW")
                              : m_reason);
    }
    return true;
}
