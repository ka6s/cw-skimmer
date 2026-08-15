QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

TARGET = cw-skimmer-gui
TEMPLATE = app

# Include paths
INCLUDEPATH += $$PWD/../include $$PWD/../src

# Source files
SOURCES += \
    main_gui.cpp \
    mainwindow.cpp \
    spectrumwidget.cpp \
    decodewidget.cpp \
    signalswidget.cpp \
    spotswidget.cpp \
    settingsdialog.cpp \
    logwidget.cpp \
    detectorworker.cpp \
    signaltracewindow.cpp \
    thresholdmorsewindow.cpp \
    maskmorsewindow.cpp \
    multichanneldecoder.cpp

HEADERS += \
    mainwindow.h \
    spectrumwidget.h \
    decodewidget.h \
    signalswidget.h \
    spotswidget.h \
    settingsdialog.h \
    logwidget.h \
    detectorworker.h \
    signaltracewindow.h \
    thresholdmorsewindow.h \
    maskmorsewindow.h \
    multichanneldecoder.h

# Link against the C detector library and build static library from sources
SOURCES += \
    ../src/audio_processor.c \
    ../src/bayesian_tree.c \
    ../src/config.c \
    ../src/cw_decoder.c \
    ../src/ditdah_decoder.c \
    ../src/decode_worker.c \
    ../src/cw_keying_detector.c \
    ../src/cw_message_validator.c \
    ../src/cw_capture.c \
    ../src/cw_detector.c \
    ../src/cwskimmer_api.c \
    ../src/perf_profile.c \
    ../src/logger.c \
    ../src/signal_analyzer.c \
    ../src/spot_reporter.c \
    ../src/tci_client.c

unix {
    LIBS += -lpthread -lm -lwebsockets
}

# Compiler flags
QMAKE_CXXFLAGS += -Wall -Wextra -std=c++17
QMAKE_CFLAGS += -Wall -Wextra -std=c99 $(shell pkg-config --cflags libwebsockets)

# Output directory
CONFIG(debug, debug|release) {
    DESTDIR = $$PWD/../bin/debug
    OBJECTS_DIR = $$PWD/../build/debug/gui
    MOC_DIR = $$PWD/../build/debug/gui
} else {
    DESTDIR = $$PWD/../bin
    OBJECTS_DIR = $$PWD/../build/gui
    MOC_DIR = $$PWD/../build/gui
}

