/**
 * @file logwidget.h
 * @brief Real-time log viewer
 */

#ifndef LOGWIDGET_H
#define LOGWIDGET_H

#include <QWidget>
#include <QTextEdit>

class LogWidget : public QWidget {
    Q_OBJECT

public:
    LogWidget(QWidget *parent = nullptr);

    void appendMessage(const QString &message, int level);
    void clear();

private:
    QString levelToString(int level);

    QTextEdit *m_textEdit;
};

#endif // LOGWIDGET_H
