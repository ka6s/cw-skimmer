/**
 * @file spotswidget.cpp
 * @brief Spot widget implementation
 */

#include "spotswidget.h"
#include <QVBoxLayout>
#include <QTableWidgetItem>
#include <QDateTime>

SpotsWidget::SpotsWidget(QWidget *parent)
    : QWidget(parent), m_maxRows(500)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({"Callsign", "Frequency (Hz)", "SNR (dB)",
                                        "Confidence (%)", "Time"});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);

    // Column widths
    m_table->setColumnWidth(0, 100);
    m_table->setColumnWidth(1, 120);
    m_table->setColumnWidth(2, 80);
    m_table->setColumnWidth(3, 100);
    m_table->setColumnWidth(4, 100);

    layout->addWidget(m_table);
    setLayout(layout);
}

void SpotsWidget::addSpot(const QString &callsign, float frequency, float snr, float confidence)
{
    // Remove old rows if we exceed max
    while (m_table->rowCount() >= m_maxRows) {
        m_table->removeRow(0);
    }

    // Insert at top
    m_table->insertRow(0);

    // Callsign
    QTableWidgetItem *callItem = new QTableWidgetItem(callsign);
    m_table->setItem(0, 0, callItem);

    // Frequency
    QTableWidgetItem *freqItem = new QTableWidgetItem(QString::number(frequency, 'f', 1));
    m_table->setItem(0, 1, freqItem);

    // SNR
    QTableWidgetItem *snrItem = new QTableWidgetItem(QString::number(snr, 'f', 1));
    m_table->setItem(0, 2, snrItem);

    // Confidence
    QTableWidgetItem *confItem = new QTableWidgetItem(QString::number(confidence * 100, 'f', 1));
    m_table->setItem(0, 3, confItem);

    // Time
    QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss");
    QTableWidgetItem *timeItem = new QTableWidgetItem(timeStr);
    m_table->setItem(0, 4, timeItem);
}

void SpotsWidget::clear()
{
    m_table->setRowCount(0);
}
