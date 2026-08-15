/**
 * @file signalswidget.h
 * @brief Real-time signal list table
 */

#ifndef SIGNALSWIDGET_H
#define SIGNALSWIDGET_H

#include <QWidget>
#include <QTableWidget>

class SignalsWidget : public QWidget {
    Q_OBJECT

public:
    SignalsWidget(QWidget *parent = nullptr);

    void addSignal(float frequency, float snr, float confidence, float tonePurity, float bandwidth);
    void clear();

private:
    QTableWidget *m_table;
    int m_maxRows;
};

#endif // SIGNALSWIDGET_H
