/**
 * @file main_gui.cpp
 * @brief GUI application entry point
 */

#include <QApplication>
#include <QFont>
#include <QMetaType>
#include "mainwindow.h"

/*
 * deskHPSDR-inspired theme (from deskhpsdr/src/css.c):
 *  - JetBrains Mono / Tahoma UI font
 *  - darkblue labels and button text
 *  - 2px borders, green hover/selected accent
 *  - pale yellow tooltips
 */
static const char kDeskHpsdrStyle[] = R"(
* {
    font-family: "JetBrains Mono", "Tahoma", "DejaVu Sans", sans-serif;
    font-size: 13px;
}
QMainWindow, QDialog, QWidget {
    background-color: #e8e8e8;
    color: #00008b;
}
QMenuBar {
    background-color: #d8d8d8;
    color: #00008b;
    font-weight: bold;
    border-bottom: 1px solid #808080;
}
QMenuBar::item:selected {
    background-color: #00ff00;
    color: #000000;
}
QMenu {
    background-color: #f0f0f0;
    color: #00008b;
    border: 1px solid #808080;
}
QMenu::item:selected {
    background-color: #00ff00;
    color: #000000;
}
QToolBar {
    background-color: #d0d0d0;
    border-bottom: 1px solid #808080;
    spacing: 6px;
    padding: 4px;
}
QStatusBar {
    background-color: #d0d0d0;
    color: #00008b;
    font-weight: bold;
}
QStatusBar QLabel {
    color: #00008b;
    font-weight: bold;
    padding: 2px 6px;
}
QPushButton {
    font-size: 14px;
    font-weight: bold;
    color: #00008b;
    background-color: #f4f4f4;
    background-image: none;
    border: 2px solid #404040;
    border-radius: 6px;
    padding: 5px 12px;
    min-height: 22px;
}
QPushButton:hover {
    background-color: #00ff00;
    color: #000000;
}
QPushButton:pressed {
    background-color: #00cc00;
    color: #000000;
}
QPushButton:checked {
    background-color: #00ff00;
    color: #000000;
    border-color: #006600;
}
QPushButton:disabled {
    color: #808080;
    background-color: #e0e0e0;
    border-color: #a0a0a0;
}
QLabel {
    color: #00008b;
}
QTabWidget::pane {
    border: 1px solid #808080;
    background-color: #f0f0f0;
}
QTabBar::tab {
    background-color: #d8d8d8;
    color: #00008b;
    font-weight: bold;
    border: 1px solid #808080;
    border-bottom: none;
    border-top-left-radius: 4px;
    border-top-right-radius: 4px;
    padding: 6px 14px;
    margin-right: 2px;
}
QTabBar::tab:selected {
    background-color: #00ff00;
    color: #000000;
}
QTabBar::tab:hover:!selected {
    background-color: #c0ffc0;
}
QCheckBox, QRadioButton {
    color: #00008b;
    font-size: 14px;
    spacing: 6px;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 16px;
    height: 16px;
    border: 2px solid #808080;
    background-color: #f8f8f8;
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background-color: #00ff00;
    border-color: #006600;
}
QSlider::groove:horizontal {
    height: 6px;
    background: #c0c0c0;
    border: 1px solid #808080;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    width: 14px;
    margin: -6px 0;
    background: #00008b;
    border: 1px solid #000050;
    border-radius: 7px;
}
QSlider::handle:horizontal:hover {
    background: #00ff00;
}
QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit {
    font-size: 14px;
    color: #00008b;
    background-color: #ffffff;
    border: 1px solid #808080;
    border-radius: 4px;
    padding: 3px 6px;
    min-height: 22px;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover {
    border-color: #00008b;
}
QComboBox QAbstractItemView {
    background-color: #ffffff;
    color: #00008b;
    selection-background-color: #00ff00;
    selection-color: #000000;
}
QGroupBox {
    font-weight: bold;
    color: #00008b;
    border: 1px solid #808080;
    border-radius: 6px;
    margin-top: 10px;
    padding-top: 8px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: #00008b;
}
QTextEdit, QPlainTextEdit {
    background-color: #101010;
    color: #e0e0e0;
    border: 1px solid #808080;
    font-family: "JetBrains Mono", "Courier New", monospace;
}
QToolTip {
    background-color: #ffffcc;
    color: #000000;
    border: 1px solid #808080;
    padding: 4px;
    font-size: 12px;
}
QScrollBar:vertical {
    background: #d8d8d8;
    width: 12px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #00008b;
    min-height: 24px;
    border-radius: 4px;
}
QScrollBar::handle:vertical:hover {
    background: #00aa00;
}
)";

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("CW Skimmer");
    app.setStyleSheet(QString::fromUtf8(kDeskHpsdrStyle));

    QFont uiFont(QStringLiteral("JetBrains Mono"));
    if (!uiFont.exactMatch()) {
        uiFont = QFont(QStringLiteral("Tahoma"));
    }
    uiFont.setPointSize(10);
    app.setFont(uiFont);

    // Register QVector<float> for cross-thread signal/slot communication
    qRegisterMetaType<QVector<float>>("QVector<float>");
    qRegisterMetaType<QVector<QVector<float>>>("QVector<QVector<float>>");

    MainWindow window;
    window.show();

    return app.exec();
}
