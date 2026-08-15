/**
 * @file logwidget.cpp
 * @brief Log widget implementation
 */

#include "logwidget.h"
#include <QVBoxLayout>
#include <QDateTime>

LogWidget::LogWidget(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    m_textEdit = new QTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_textEdit->setFont(QFont("Courier New", 9));

    layout->addWidget(m_textEdit);
    setLayout(layout);
}

void LogWidget::appendMessage(const QString &message, int level)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString levelStr = levelToString(level);

    QString formattedMessage = QString("[%1] %2: %3").arg(timestamp, levelStr, message);

    // Add color coding based on level
    QString html;
    switch (level) {
    case 0:  // DEBUG
        html = QString("<font color='gray'>%1</font>").arg(formattedMessage);
        break;
    case 1:  // INFO
        html = QString("<font color='black'>%1</font>").arg(formattedMessage);
        break;
    case 2:  // WARNING
        html = QString("<font color='orange'>%1</font>").arg(formattedMessage);
        break;
    case 3:  // ERROR
        html = QString("<font color='red'>%1</font>").arg(formattedMessage);
        break;
    default:
        html = formattedMessage;
    }

    m_textEdit->append(html);

    // Keep only last 1000 lines
    QTextDocument *doc = m_textEdit->document();
    if (doc->blockCount() > 1000) {
        QTextCursor cursor(doc);
        cursor.movePosition(QTextCursor::Start);
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
    }
}

void LogWidget::clear()
{
    m_textEdit->clear();
}

QString LogWidget::levelToString(int level)
{
    switch (level) {
    case 0:
        return "DEBUG";
    case 1:
        return "INFO ";
    case 2:
        return "WARN ";
    case 3:
        return "ERROR";
    default:
        return "UNKN ";
    }
}
