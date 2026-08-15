# CW Skimmer for Linux - Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -fPIC -std=c99 $(shell pkg-config --cflags libwebsockets)
CXXFLAGS = -Wall -Wextra -O2 -fPIC -std=c++17
LDFLAGS = -lm -lpthread $(shell pkg-config --libs libwebsockets)
DEBUG_FLAGS = -g -O0 -DDEBUG

SRCDIR = src
INCDIR = include
OBJDIR = build
BINDIR = bin
TESTDIR = tests
GUIDIR = gui

# Check for Qt5
QT5_PATH := $(shell which qmake 2>/dev/null || echo "")
ifeq ($(QT5_PATH),)
	HAS_QT5 = 0
else
	HAS_QT5 = 1
endif

# Main executable
TARGET = $(BINDIR)/cw-skimmer
GUI_TARGET = $(BINDIR)/cw-skimmer-gui

# Source files ( excluded — ditdah decoder replaces DeepCW)
SOURCES = $(filter-out $(SRCDIR)/deepcw_engine.c,$(wildcard $(SRCDIR)/*.c))
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

.PHONY: all clean debug test replay synth help gui gui-clean

all: $(DIRS) $(TARGET) shared-lib

shared-lib: $(OBJDIR)/libcwskimmer_c.so

debug: CFLAGS += $(DEBUG_FLAGS)
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: clean $(DIRS) $(TARGET) shared-lib

$(DIRS):
	mkdir -p $@

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "✓ Built: $@"

# Build shared library for GUI
$(OBJDIR)/libcwskimmer_c.so: $(OBJECTS)
	$(CC) -shared -fPIC -o $(BINDIR)/libcwskimmer_c.so $^ $(LDFLAGS)
	@echo "✓ Built: $(BINDIR)/libcwskimmer_c.so"

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

$(OBJDIR)/%.o: $(TESTDIR)/%.c
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
	$(CC) -o $@ $^ $(LDFLAGS) -L/usr/lib64 -lcap
	@echo "✓ Built: $@"

# GUI targets
gui: shared-lib
ifeq ($(HAS_QT5),1)
	@echo "✓ Building Qt5 GUI..."
	@cd $(GUIDIR) && qmake -o Makefile.qt5 cw_skimmer_gui.pro && make -f Makefile.qt5
	@cp $(GUIDIR)/bin/cw-skimmer-gui $(BINDIR)/cw-skimmer-gui 2>/dev/null || true
	@echo "✓ GUI built successfully"
else
	@echo "✗ Qt5 not found. Install with: sudo apt-get install qt5-default"
endif

gui-clean:
	@cd $(GUIDIR) && rm -f Makefile.qt5 && make -f Makefile.qt5 clean 2>/dev/null || true
	@rm -rf $(GUIDIR)/bin $(GUIDIR)/build

clean: gui-clean
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "✓ Cleaned"

help:
	@echo "CW Skimmer Makefile targets:"
	@echo "  make all      - Build the main CLI executable and shared library"
	@echo "  make gui      - Build the Qt5 GUI (requires Qt5)"
	@echo "  make debug    - Build CLI with debug symbols"
	@echo "  make test     - Build and run tests"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make help     - Show this help"
