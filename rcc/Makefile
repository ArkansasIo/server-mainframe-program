# ===========================================================================
#  Railway Control Program (RCP) - build file
#
#  Plain POSIX make. No external dependencies beyond the C standard library
#  and the platform socket API for the remote panel.
#
#    make            build ./build/rcp and ./build/rcp-tests
#    make run        build and start the interactive control console
#    make test       build and run the unit tests
#    make sim        run a 60 second headless simulation
#    make clean      remove build artefacts
#
#  Override the compiler/standard with e.g.
#    make CC=gcc CFLAGS="-O2 -std=c17"
# ===========================================================================

CC      ?= cc
STD     ?= -std=c17
WARN    ?= -Wall -Wextra -Wshadow -Wconversion -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes
OPT     ?= -O2
DEBUG   ?= -g
CFLAGS  ?= $(STD) $(WARN) $(OPT) $(DEBUG) -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS  ?=

BUILD   := build
SRCDIR  := src
TESTDIR := tests

# ---------------------------------------------------------------------------
# Platform specific bits
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
    EXE := .exe
    # MinGW / clang targeting Windows
    LDLIBS += -lws2_32
    CFLAGS += -D_WIN32_WINNT=0x0601
    MKDIR  := if not exist "$(BUILD)" mkdir "$(BUILD)"
else
    EXE :=
    LDLIBS += -lpthread
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        CFLAGS += -D_DARWIN_C_SOURCE
    endif
endif

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
CORE_SRC := \
    $(SRCDIR)/main.c \
    $(SRCDIR)/rcp_console.c \
    $(SRCDIR)/rcp_signal.c \
    $(SRCDIR)/rcp_track.c \
    $(SRCDIR)/rcp_point.c \
    $(SRCDIR)/rcp_train.c \
    $(SRCDIR)/rcp_sensor.c \
    $(SRCDIR)/rcp_route.c \
    $(SRCDIR)/rcp_interlock.c \
    $(SRCDIR)/rcp_emergency.c \
    $(SRCDIR)/rcp_eventlog.c \
    $(SRCDIR)/rcp_sim.c \
    $(SRCDIR)/rcp_layout.c \
    $(SRCDIR)/rcp_ui.c \
    $(SRCDIR)/rcp_persist.c \
    $(SRCDIR)/rcp_util.c

TEST_SRC := \
    $(TESTDIR)/test_main.c \
    $(TESTDIR)/test_interlock.c \
    $(TESTDIR)/test_route.c \
    $(TESTDIR)/test_points.c \
    $(TESTDIR)/test_signals.c \
    $(TESTDIR)/test_sim.c \
    $(TESTDIR)/test_eventlog.c \
    $(TESTDIR)/test_util.c

LIB_SRC := $(filter-out $(SRCDIR)/main.c,$(CORE_SRC))

CORE_OBJ := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(CORE_SRC))
LIB_OBJ  := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(LIB_SRC))
TEST_OBJ := $(patsubst $(TESTDIR)/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))

TARGET   := $(BUILD)/rcp$(EXE)
TESTBIN  := $(BUILD)/rcp-tests$(EXE)

INCLUDES := -Iinclude

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all run test sim clean fmt help

all: $(TARGET) $(TESTBIN)

$(BUILD):
	@$(MKDIR)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/tests/%.o: $(TESTDIR)/%.c | $(BUILD)
	@if not exist "$(BUILD)\tests" mkdir "$(BUILD)\tests"
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(CORE_OBJ)
	$(CC) $(CFLAGS) $(CORE_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(TESTBIN): $(TEST_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(TEST_OBJ) $(LIB_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	$(TARGET) --console

sim: $(TARGET)
	$(TARGET) --simulate 60 --scenario peak

test: $(TESTBIN)
	$(TESTBIN)

clean:
	@if exist "$(BUILD)" rmdir /s /q "$(BUILD)"

help:
	@echo RCP targets: all run test sim clean
