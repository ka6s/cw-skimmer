/**
 * @file settingsdialog.cpp
 * @brief Settings dialog implementation
 */

#include "settingsdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include <algorithm>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , m_radioHostEdit(nullptr)
    , m_radioPortSpinBox(nullptr)
    , m_radioProtocolCombo(nullptr)
    , m_thresholdSpinBox(nullptr)
    , m_minSnrSpinBox(nullptr)
    , m_decodeChannelsSpinBox(nullptr)
    , m_callsignEdit(nullptr)
{
    setWindowTitle("Settings");
    setModal(true);
    setMinimumWidth(420);
    createUI();
    loadSettings();
}

void SettingsDialog::createUI()
{
    /* Parent the layout to this dialog exactly once (do not also call setLayout). */
    auto *mainLayout = new QVBoxLayout(this);

    // Radio settings group
    auto *radioGroup = new QGroupBox(QStringLiteral("Radio Connection"), this);
    auto *radioLayout = new QVBoxLayout(radioGroup);

    auto *hostLayout = new QHBoxLayout();
    hostLayout->addWidget(new QLabel(QStringLiteral("Radio Host:"), radioGroup));
    m_radioHostEdit = new QLineEdit(radioGroup);
    m_radioHostEdit->setText(QStringLiteral("192.168.2.146"));
    hostLayout->addWidget(m_radioHostEdit);
    radioLayout->addLayout(hostLayout);

    auto *portLayout = new QHBoxLayout();
    portLayout->addWidget(new QLabel(QStringLiteral("Radio Port:"), radioGroup));
    m_radioPortSpinBox = new QSpinBox(radioGroup);
    m_radioPortSpinBox->setRange(1, 65535);
    m_radioPortSpinBox->setValue(50001);
    portLayout->addWidget(m_radioPortSpinBox);
    portLayout->addStretch();
    radioLayout->addLayout(portLayout);

    auto *protocolLayout = new QHBoxLayout();
    protocolLayout->addWidget(new QLabel(QStringLiteral("Protocol:"), radioGroup));
    m_radioProtocolCombo = new QComboBox(radioGroup);
    m_radioProtocolCombo->addItem(QStringLiteral("TCP/IP"), QStringLiteral("tcp"));
    m_radioProtocolCombo->addItem(QStringLiteral("WebSocket"), QStringLiteral("websocket"));
    m_radioProtocolCombo->setCurrentIndex(1);
    protocolLayout->addWidget(m_radioProtocolCombo);
    protocolLayout->addStretch();
    radioLayout->addLayout(protocolLayout);

    mainLayout->addWidget(radioGroup);

    // Detection settings group
    auto *detectionGroup = new QGroupBox(QStringLiteral("Detection Settings"), this);
    auto *detectionLayout = new QVBoxLayout(detectionGroup);

    auto *thresholdLayout = new QHBoxLayout();
    thresholdLayout->addWidget(new QLabel(QStringLiteral("Detection Threshold (%):"), detectionGroup));
    m_thresholdSpinBox = new QSpinBox(detectionGroup);
    m_thresholdSpinBox->setRange(0, 100);
    m_thresholdSpinBox->setValue(14);
    thresholdLayout->addWidget(m_thresholdSpinBox);
    thresholdLayout->addStretch();
    detectionLayout->addLayout(thresholdLayout);

    auto *snrLayout = new QHBoxLayout();
    snrLayout->addWidget(new QLabel(QStringLiteral("Min SNR (dB):"), detectionGroup));
    m_minSnrSpinBox = new QDoubleSpinBox(detectionGroup);
    m_minSnrSpinBox->setRange(0.5, 30.0);
    m_minSnrSpinBox->setSingleStep(0.5);
    m_minSnrSpinBox->setDecimals(1);
    m_minSnrSpinBox->setValue(2.0);
    snrLayout->addWidget(m_minSnrSpinBox);
    snrLayout->addStretch();
    detectionLayout->addLayout(snrLayout);

    auto *decodeChLayout = new QHBoxLayout();
    decodeChLayout->addWidget(new QLabel(QStringLiteral("CW Decode channels:"), detectionGroup));
    m_decodeChannelsSpinBox = new QSpinBox(detectionGroup);
    m_decodeChannelsSpinBox->setRange(1, 16);
    m_decodeChannelsSpinBox->setValue(1);
    m_decodeChannelsSpinBox->setToolTip(
        QStringLiteral(
            "Number of parallel Morse decode threads in the CW Decode panel\n"
            "(strongest signals). Each channel has independent Auto thr / WPM."));
    decodeChLayout->addWidget(m_decodeChannelsSpinBox);
    decodeChLayout->addStretch();
    detectionLayout->addLayout(decodeChLayout);

    mainLayout->addWidget(detectionGroup);

    // Reporting settings group
    auto *reportGroup = new QGroupBox(QStringLiteral("Spot Reporting"), this);
    auto *reportLayout = new QVBoxLayout(reportGroup);

    auto *callLayout = new QHBoxLayout();
    callLayout->addWidget(new QLabel(QStringLiteral("Reporter Callsign:"), reportGroup));
    m_callsignEdit = new QLineEdit(reportGroup);
    m_callsignEdit->setText(QStringLiteral("CWSKIMMER"));
    callLayout->addWidget(m_callsignEdit);
    reportLayout->addLayout(callLayout);

    mainLayout->addWidget(reportGroup);
    mainLayout->addStretch(1);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    auto *okButton = new QPushButton(QStringLiteral("OK"), this);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(okButton);
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);
    mainLayout->addLayout(buttonLayout);
}

void SettingsDialog::loadSettings()
{
    /* Defaults already set in createUI(); values can be overridden via set* before exec. */
}

QString SettingsDialog::getRadioHost() const
{
    return m_radioHostEdit ? m_radioHostEdit->text() : QStringLiteral("192.168.2.146");
}

int SettingsDialog::getRadioPort() const
{
    return m_radioPortSpinBox ? m_radioPortSpinBox->value() : 50001;
}

QString SettingsDialog::getRadioProtocol() const
{
    if (!m_radioProtocolCombo) {
        return QStringLiteral("websocket");
    }
    const QVariant data = m_radioProtocolCombo->currentData();
    if (data.isValid()) {
        return data.toString();
    }
    return QStringLiteral("websocket");
}

int SettingsDialog::getDetectionThreshold() const
{
    return m_thresholdSpinBox ? m_thresholdSpinBox->value() : 14;
}

double SettingsDialog::getMinSnrDb() const
{
    return m_minSnrSpinBox ? m_minSnrSpinBox->value() : 2.0;
}

QString SettingsDialog::getCallsign() const
{
    return m_callsignEdit ? m_callsignEdit->text() : QStringLiteral("CWSKIMMER");
}

int SettingsDialog::getDecodeChannels() const
{
    return m_decodeChannelsSpinBox ? m_decodeChannelsSpinBox->value() : 1;
}

void SettingsDialog::setDecodeChannels(int n)
{
    if (!m_decodeChannelsSpinBox) {
        return;
    }
    if (n < 1) {
        n = 1;
    }
    if (n > 16) {
        n = 16;
    }
    m_decodeChannelsSpinBox->setValue(n);
}
