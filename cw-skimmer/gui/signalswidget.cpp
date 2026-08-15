/**
 * @file signalswidget.cpp
 * @brief Signal table implementation
 */

#include "signalswidget.h"
#include <QVBoxLayout>
#include <QTableWidgetItem>
#include <QDateTime>

SignalsWidget::SignalsWidget(QWidget *parent)
    : QWidget(parent), m_maxRows(1000)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({"Frequency (Hz)", "SNR (dB)", "Confidence (%)",
                                        "Tone Purity", "Bandwidth (Hz)", "Callsign", "Time"});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);

    // Column widths
    m_table->setColumnWidth(0, 120);
    m_table->setColumnWidth(1, 80);
    m_table->setColumnWidth(2, 100);
    m_table->setColumnWidth(3, 100);
    m_table->setColumnWidth(4, 100);
    m_table->setColumnWidth(5, 80);
    m_table->setColumnWidth(6, 100);

    layout->addWidget(m_table);
    setLayout(layout);
}

void SignalsWidget::addSignal(float frequency, float snr, float confidence,
                              float tonePurity, float bandwidth)
{
    // Remove old rows if we exceed max
    while (m_table->rowCount() >= m_maxRows) {
        m_table->removeRow(0);
    }

    // Insert at top
    m_table->insertRow(0);

    // Frequency
    QTableWidgetItem *freqItem = new QTableWidgetItem(QString::number(frequency, 'f', 1));
    m_table->setItem(0, 0, freqItem);

    // SNR
    QTableWidgetItem *snrItem = new QTableWidgetItem(QString::number(snr, 'f', 1));
    m_table->setItem(0, 1, snrItem);

    // Confidence
    QTableWidgetItem *confItem = new QTableWidgetItem(QString::number(confidence * 100, 'f', 1));
    m_table->setItem(0, 2, confItem);

    // Tone Purity
    QTableWidgetItem *purityItem = new QTableWidgetItem(QString::number(tonePurity, 'f', 3));
    m_table->setItem(0, 3, purityItem);

    // Bandwidth
    QTableWidgetItem *bwItem = new QTableWidgetItem(QString::number(bandwidth, 'f', 1));
    m_table->setItem(0, 4, bwItem);

    // Callsign (placeholder)
    QTableWidgetItem *callItem = new QTableWidgetItem("---");
    m_table->setItem(0, 5, callItem);

    // Time
    QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss");
    QTableWidgetItem *timeItem = new QTableWidgetItem(timeStr);
    m_table->setItem(0, 6, timeItem);
}

void SignalsWidget::clear()
{
    m_table->setRowCount(0);
}
