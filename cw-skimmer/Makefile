# CW Skimmer
# The compiler target picks the window system.
# Linux gcc builds the command-line detector, and `make gui` builds the Qt5 window.
# MinGW (Windows 10) builds a native Win32 window and does not call Qt.
# Force a side with `make HOST_OS=linux` or `make HOST_OS=windows`.

CC = gcc
CXX = g++

ifeq ($(origin HOST_OS), undefined)
  TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
  ifneq ($(findstring mingw,$(TARGET_TRIPLE)),)
    HOST_OS := windows
  else ifneq ($(findstring windows,$(TARGET_TRIPLE)),)
    HOST_OS := windows
  else
    UNAME_S := $(shell uname -s 2>/dev/null)
    ifneq ($(findstring MINGW,$(UNAME_S)),)
      HOST_OS := windows
    else ifneq ($(findstring MSYS,$(UNAME_S)),)
      HOST_OS := windows
    else ifeq ($(OS),Windows_NT)
      ifneq ($(findstring msys,$(TARGET_TRIPLE)),)
        HOST_OS := msys
      else
        HOST_OS := windows
      endif
    else
      HOST_OS := linux
    endif
  endif
endif

LWS_CFLAGS := $(shell pkg-config --cflags libwebsockets 2>/dev/null)
LWS_LIBS := $(shell pkg-config --libs libwebsockets 2>/dev/null)
ifeq ($(LWS_LIBS),)
  LWS_LIBS := -lwebsockets
endif

CSTD := -std=c99
WIN_CFLAGS :=
WIN_LIBS :=
ifeq ($(HOST_OS),windows)
  CSTD := -std=gnu99
  WIN_CFLAGS := -D_WIN32_WINNT=0x0A00 -include src/win_compat.h
  WIN_LIBS := -lws2_32 -luser32 -lgdi32
endif

CFLAGS = -Wall -Wextra -O2 -fPIC $(CSTD) $(WIN_CFLAGS) $(LWS_CFLAGS)
CXXFLAGS = -Wall -Wextra -O2 -fPIC -std=c++17
LDFLAGS = -lm -lpthread $(LWS_LIBS) $(WIN_LIBS)
DEBUG_FLAGS = -g -O0 -DDEBUG

SRCDIR = src
INCDIR = include
OBJDIR = build
BINDIR = bin
TESTDIR = tests
GUIDIR = gui

# Check for Qt5 (Linux window only)
QT5_PATH := $(shell which qmake 2>/dev/null || echo "")
ifeq ($(QT5_PATH),)
	HAS_QT5 = 0
else
	HAS_QT5 = 1
endif

ifeq ($(HOST_OS),windows)
TARGET = $(BINDIR)/cw-skimmer.exe
GUI_TARGET = $(BINDIR)/cw-skimmer-gui.exe
REPLAY_EXTRA =
else
TARGET = $(BINDIR)/cw-skimmer
GUI_TARGET = $(BINDIR)/cw-skimmer-gui
REPLAY_EXTRA = -L/usr/lib64 -lcap
endif

SOURCES = $(wildcard $(SRCDIR)/*.c)
HEADERS = $(wildcard $(SRCDIR)/*.h)
# Include API wrapper in build
API_SOURCES = $(SRCDIR)/cwskimmer_api.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Test executable
TEST_TARGET = $(BINDIR)/test_detector
REPLAY_TARGET = $(BINDIR)/test_replay_capture
SYNTH_TARGET = $(BINDIR)/test_synth_capture
REPLAY_SOURCES = $(TESTDIR)/test_replay_capture.c $(API_SOURCES) $(SRCDIR)/cw_capture.c \
	$(SRCDIR)/tci_client.c $(SRCDIR)/audio_processor.c $(SRCDIR)/cw_detector.c \
	$(SRCDIR)/bayesian_tree.c $(SRCDIR)/cw_decoder.c $(SRCDIR)/ditdah_decoder.c \
	$(SRCDIR)/decode_worker.c $(SRCDIR)/cw_message_validator.c $(SRCDIR)/config.c \
	$(SRCDIR)/logger.c $(SRCDIR)/signal_analyzer.c $(SRCDIR)/spot_reporter.c \
	$(SRCDIR)/perf_profile.c
REPLAY_OBJECTS = $(REPLAY_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
REPLAY_OBJECTS := $(REPLAY_OBJECTS:$(TESTDIR)/%.c=$(OBJDIR)/%.o)
CAPTURE ?= captures/cwcap_20260704_091418_f14071433.cwcap
TEST_SOURCES = $(TESTDIR)/test_detector.c $(SRCDIR)/cw_detector.c $(SRCDIR)/cw_decoder.c \
	$(SRCDIR)/ditdah_decoder.c $(SRCDIR)/bayesian_tree.c $(SRCDIR)/logger.c \
	$(SRCDIR)/config.c $(SRCDIR)/cw_message_validator.c
TEST_OBJECTS = $(TEST_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
TEST_OBJECTS := $(TEST_OBJECTS:$(TESTDIR)/%.c=$(OBJDIR)/%.o)

# Directories
DIRS = $(OBJDIR) $(BINDIR)

.PHONY: all clean debug test replay synth help gui gui-clean training-gen training-data ort deepcw-test radio-test win-gui cli host-check

ORT_DIR := third_party/onnxruntime
ORT_LIB := $(ORT_DIR)/lib/libonnxruntime.so
ORT_TGZ := /tmp/onnxruntime-linux-x64-1.31.0.tgz
DEEPCW_TEST := $(BINDIR)/test_deepcw_decode
RADIO_TEST := $(BINDIR)/test_radio_control

TRAIN_GEN = $(BINDIR)/cw-training-gen

ifeq ($(HOST_OS),windows)
all: $(DIRS) win-gui
else ifeq ($(HOST_OS),msys)
all:
	@echo "This gcc is the MSYS compiler. Open the MSYS2 MinGW 64-bit shell and run make there."
	@exit 1
else
all: $(DIRS) $(TARGET) shared-lib
endif

shared-lib: $(OBJDIR)/libcwskimmer_c.so

cli: $(DIRS) $(TARGET)

debug: CFLAGS += $(DEBUG_FLAGS)
debug: CXXFLAGS += $(DEBUG_FLAGS)
ifeq ($(HOST_OS),windows)
debug: clean $(DIRS) win-gui
else
debug: clean $(DIRS) $(TARGET) shared-lib
endif

$(DIRS):
	mkdir -p $@

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "✓ Built: $@"

# Build shared library for GUI
$(OBJDIR)/libcwskimmer_c.so: $(OBJECTS)
	$(CC) -shared -fPIC -o $(BINDIR)/libcwskimmer_c.so $^ $(LDFLAGS)
	@echo "✓ Built: $(BINDIR)/libcwskimmer_c.so"

# Rebuild objects when the host changes, so Linux .o files are not linked into the Win32 exe.
host-check: | $(OBJDIR)
	@echo $(HOST_OS) > $(OBJDIR)/host.tmp
	@if ! cmp -s $(OBJDIR)/host.tmp $(OBJDIR)/host.tag 2>/dev/null; then mv $(OBJDIR)/host.tmp $(OBJDIR)/host.tag; else rm -f $(OBJDIR)/host.tmp; fi

$(OBJDIR)/host.tag: host-check
	@test -f $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(OBJDIR)/host.tag
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

$(OBJDIR)/%.o: $(TESTDIR)/%.c $(OBJDIR)/host.tag
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

test: $(DIRS) $(TEST_TARGET)
	@echo "Running tests..."
	@./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "✓ Built: $@"

replay: $(DIRS) $(REPLAY_TARGET)
	@echo "Replaying $(CAPTURE) ..."
	@./$(REPLAY_TARGET) $(CAPTURE)

synth: $(DIRS) $(SYNTH_TARGET)
	@./$(SYNTH_TARGET)

SYNTH_OBJECTS = $(OBJDIR)/test_synth_capture.o $(filter-out $(OBJDIR)/test_replay_capture.o,$(REPLAY_OBJECTS))

$(SYNTH_TARGET): $(SYNTH_OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "✓ Built: $@"

$(OBJDIR)/test_synth_capture.o: $(TESTDIR)/test_synth_capture.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

$(REPLAY_TARGET): $(REPLAY_OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS) $(REPLAY_EXTRA)
	@echo "✓ Built: $@"

# ONNX Runtime 1.31 CPU build used by the DeepCW pane.
ort: $(ORT_LIB)

$(ORT_LIB):
	mkdir -p /tmp/ort-dl
	curl -L --fail -o $(ORT_TGZ) https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/onnxruntime-linux-x64-1.31.0.tgz
	rm -rf /tmp/ort-dl/onnxruntime-linux-x64-1.31.0
	tar -xzf $(ORT_TGZ) -C /tmp/ort-dl
	rm -rf $(ORT_DIR)/include $(ORT_DIR)/lib
	mkdir -p $(ORT_DIR)
	mv /tmp/ort-dl/onnxruntime-linux-x64-1.31.0/include $(ORT_DIR)/include
	mv /tmp/ort-dl/onnxruntime-linux-x64-1.31.0/lib $(ORT_DIR)/lib
	@echo "✓ ONNX Runtime in $(ORT_DIR)"

deepcw-test: $(ORT_LIB) $(DEEPCW_TEST)
	./$(DEEPCW_TEST) models/model.onnx models/model.onnx.json

$(DEEPCW_TEST): tests/test_deepcw_decode.cpp src/deepcw_engine.cpp src/deepcw_engine.h $(ORT_LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -I$(ORT_DIR)/include tests/test_deepcw_decode.cpp src/deepcw_engine.cpp \
		-L$(ORT_DIR)/lib -lonnxruntime -Wl,-rpath,$(CURDIR)/$(ORT_DIR)/lib -o $@

# Command formatting for tune and CW send. Not part of the default build.
radio-test: $(DIRS) $(RADIO_TEST)
	./$(RADIO_TEST)

$(RADIO_TEST): tests/test_radio_control.c src/radio_control.c src/tci_client.c src/logger.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) tests/test_radio_control.c src/radio_control.c src/tci_client.c src/logger.c \
		$(LDFLAGS) -o $@

# Window. Linux uses Qt5. Windows uses Win32 (user32/gdi32), not Qt.
WIN_OBJECTS = $(filter-out $(OBJDIR)/main.o,$(OBJECTS))

$(OBJDIR)/cwskimmer_win.o: win/cwskimmer_win.c $(OBJDIR)/host.tag | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

ifeq ($(HOST_OS),windows)
win-gui: $(DIRS) $(WIN_OBJECTS) $(OBJDIR)/cwskimmer_win.o
	$(CC) -mwindows -o $(GUI_TARGET) $(WIN_OBJECTS) $(OBJDIR)/cwskimmer_win.o $(LDFLAGS)
	@echo "✓ Built: $(GUI_TARGET)"
gui: win-gui
else ifeq ($(HOST_OS),msys)
win-gui gui:
	@echo "This gcc is the MSYS compiler. Open the MSYS2 MinGW 64-bit shell and run make there."
	@exit 1
else
win-gui:
	@echo "The Win32 window is the Windows build. On Linux, run make gui."
gui: shared-lib $(ORT_LIB)
ifeq ($(HAS_QT5),1)
	@echo "✓ Building Qt5 GUI..."
	@cd $(GUIDIR) && qmake -o Makefile.qt5 cw_skimmer_gui.pro && make -f Makefile.qt5
	@cp $(GUIDIR)/bin/cw-skimmer-gui $(BINDIR)/cw-skimmer-gui 2>/dev/null || true
	@echo "✓ GUI built successfully"
else
	@echo "✗ Qt5 not found. Install with: sudo apt-get install qt5-default"
endif
endif

gui-clean:
ifeq ($(HOST_OS),windows)
	rm -f $(GUI_TARGET) $(OBJDIR)/cwskimmer_win.o
else
	@cd $(GUIDIR) && rm -f Makefile.qt5 && make -f Makefile.qt5 clean 2>/dev/null || true
	@rm -rf $(GUIDIR)/bin $(GUIDIR)/build
endif

clean: gui-clean
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "✓ Cleaned"

training-gen: $(DIRS)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $(TRAIN_GEN) tools/cw_training_gen.c -lm
	@echo "✓ Built: $(TRAIN_GEN)"

training-data: training-gen
	./$(TRAIN_GEN) training

help:
	@echo "CW Skimmer ($(HOST_OS))"
ifeq ($(HOST_OS),windows)
	@echo "  make / make gui     - Build the native Win32 window ($(GUI_TARGET))"
	@echo "  make cli            - Build the command-line detector"
else
	@echo "  make all            - Build the main CLI executable and shared library"
	@echo "  make gui            - Build the Qt5 GUI (requires Qt5)"
	@echo "  make cli            - Build the command-line detector"
endif
	@echo "  make ort            - Download ONNX Runtime for the DeepCW pane (Linux)"
	@echo "  make deepcw-test    - Decode a synthetic tone with DeepCW"
	@echo "  make radio-test     - Check tune and CW command formatting"
	@echo "  make debug          - Rebuild with debug symbols"
	@echo "  make test           - Build and run tests"
	@echo "  make training-gen   - Build the TCI training-file generator"
	@echo "  make training-data  - Write training/set_01..10 TCI streams and transcripts"
	@echo "  make clean          - Remove build artifacts (keeps training/)"
	@echo "  make help           - Show this help"
