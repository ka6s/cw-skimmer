#ifndef CWKEYBOARD_H
#define CWKEYBOARD_H

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

/**
 * Type-ahead CW keyboard under the Morse decoder.
 * Characters queue locally and flush as a short string. Esc or Stop aborts.
 */
class CwKeyboard : public QWidget {
    Q_OBJECT

public:
    explicit CwKeyboard(QWidget *parent = nullptr);

    void setWpm(int wpm);
    int wpm() const;
    void setTransportLabel(const QString &label);
    /** armed is false during stop and training playback. */
    void setArmed(bool armed, const QString &reason);
    /** Send text now, without touching the type-ahead line. */
    void sendImmediate(const QString &text);

signals:
    void textReady(const QString &text);
    void stopRequested();
    void wpmChanged(int wpm);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void flushPending();

private:
    void appendText(const QString &text);
    void showPending();
    void rememberSent(const QString &text);

    QLabel *m_sent;
    QLabel *m_status;
    QLineEdit *m_input;
    QSpinBox *m_wpm;
    QPushButton *m_stop;
    QTimer *m_flush;
    QString m_pending;
    QString m_reason;
    bool m_armed;
};

#endif
