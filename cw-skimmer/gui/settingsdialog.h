/**
 * @file settingsdialog.h
 * @brief Configuration dialog
 */

#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QDoubleSpinBox>

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(QWidget *parent = nullptr);

    QString getRadioHost() const;
    int getRadioPort() const;
    QString getRadioProtocol() const;
    int getDetectionThreshold() const;
    double getMinSnrDb() const;
    QString getCallsign() const;
    /** Parallel CW Decode channels (1…16). */
    int getDecodeChannels() const;
    void setDecodeChannels(int n);

private:
    void createUI();
    void loadSettings();

    QLineEdit *m_radioHostEdit;
    QSpinBox *m_radioPortSpinBox;
    QComboBox *m_radioProtocolCombo;
    QSpinBox *m_thresholdSpinBox;
    QDoubleSpinBox *m_minSnrSpinBox;
    QSpinBox *m_decodeChannelsSpinBox;
    QLineEdit *m_callsignEdit;
};

#endif // SETTINGSDIALOG_H

