/**
 * @file spotswidget.h
 * @brief Spot report history display
 */

#ifndef SPOTSWIDGET_H
#define SPOTSWIDGET_H

#include <QWidget>
#include <QTableWidget>

class SpotsWidget : public QWidget {
    Q_OBJECT

public:
    SpotsWidget(QWidget *parent = nullptr);

    void addSpot(const QString &callsign, float frequency, float snr, float confidence);
    void clear();

private:
    QTableWidget *m_table;
    int m_maxRows;
};

#endif // SPOTSWIDGET_H
