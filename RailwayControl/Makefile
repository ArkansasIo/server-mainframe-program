# ===========================================================================
#  RailControl - Railway Traffic Management & Interlocking Simulator
#
#  Plain POSIX make. No external dependencies beyond the C standard library
#  and the platform APIs (Win32 multimedia for the alarm sound).
#
#    make            build build/rcp[.exe] and the test runner
#    make run        build and start the interactive control console
#    make sim        run a 60 second headless simulation
#    make test       build and run the unit tests
#    make clean      remove build artefacts
#
#  Override the toolchain with e.g.
#    make CC=gcc
#    make CC="clang -std=c17" OPT=-O0
# ===========================================================================

NAME    := RailControl
CC      ?= cc
STD     ?= -std=c17
WARN    ?= -Wall -Wextra -Wshadow -Wconversion -Wpointer-arith \
           -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual
OPT     ?= -O2
DEBUG   ?= -g
CFLAGS  ?= $(STD) $(WARN) $(OPT) $(DEBUG) -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=

BUILD   := build
OBJDIR  := $(BUILD)/obj
SRCDIR  := src
INCDIR  := include
TESTDIR := tests

# ---------------------------------------------------------------------------
# Platform specifics
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
    EXE      := .exe
    RM       := cmd /c if exist "$(1)" rmdir /s /q "$(1)"
    MKDIR    := cmd /c if not exist "$(1)" mkdir "$(1)"
    MKDIRP   := cmd /c if not exist "$(1)" mkdir "$(1)"
    LDLIBS   := -lwinmm -lws2_32
    CFLAGS   += -D_WIN32_WINNT=0x0601
else
    EXE      :=
    MKDIR    := mkdir -p $(1)
    MKDIRP   := mkdir -p $(1)
    LDLIBS   := -lm
    UNAME_S  := $(shell uname -s 2>/dev/null)
    ifeq ($(UNAME_S),Darwin)
        CFLAGS += -D_DARWIN_C_SOURCE
        LDLIBS += -lpthread
    else
        LDLIBS += -lpthread -lrt
    endif
endif

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
CORE_SRC := \
    $(SRCDIR)/main.c \
    $(SRCDIR)/railway_api.c \
    $(SRCDIR)/rc_account.c \
    $(SRCDIR)/rc_graph.c \
    $(SRCDIR)/rc_persist.c \
    $(SRCDIR)/rc_settings.c \
    $(SRCDIR)/rcp_console.c \
    $(SRCDIR)/rcp_emergency.c \
    $(SRCDIR)/rcp_eventlog.c \
    $(SRCDIR)/rcp_interlock.c \
    $(SRCDIR)/rcp_layout.c \
    $(SRCDIR)/rcp_names.c \
    $(SRCDIR)/rcp_persist.c \
    $(SRCDIR)/rcp_point.c \
    $(SRCDIR)/rcp_route.c \
    $(SRCDIR)/rcp_sensor.c \
    $(SRCDIR)/rcp_signal.c \
    $(SRCDIR)/rcp_sim.c \
    $(SRCDIR)/rcp_track.c \
    $(SRCDIR)/rcp_train.c \
    $(SRCDIR)/rcp_ui.c \
    $(SRCDIR)/rcp_util.c \
    $(SRCDIR)/conjob.c

TEST_SRC := \
    $(TESTDIR)/test_main.c \
    $(TESTDIR)/test_interlock.c \
    $(TESTDIR)/test_routes.c \
    $(TESTDIR)/test_points.c \
    $(TESTDIR)/test_signals.c \
    $(TESTDIR)/test_trains.c \
    $(TESTDIR)/test_tracks.c \
    $(TESTDIR)/test_emergency.c \
    $(TESTDIR)/test_conjob.c \
    $(TESTDIR)/test_util.c

LIB_SRC  := $(filter-out $(SRCDIR)/main.c,$(CORE_SRC))
CORE_OBJ := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CORE_SRC))
LIB_OBJ  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_SRC))
TEST_OBJ := $(patsubst $(TESTDIR)/%.c,$(OBJDIR)/tests/%.o,$(TEST_SRC))

TARGET   := $(BUILD)/rcp$(EXE)
TESTBIN  := $(BUILD)/rcp-tests$(EXE)
INCLUDES := -I$(INCDIR)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all run sim test clean help dirs

all: $(TARGET)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@$(call MKDIRP,$(OBJDIR))
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/tests/%.o: $(TESTDIR)/%.c
	@$(call MKDIRP,$(OBJDIR)/tests)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(CORE_OBJ)
	$(CC) $(CFLAGS) $(CORE_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(TESTBIN): $(TEST_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(TEST_OBJ) $(LIB_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	$(TARGET) --user admin --password Admin#2024

sim: $(TARGET)
	$(TARGET) --simulate 60 --scenario peak --user admin --password Admin#2024

test: $(TESTBIN)
	$(TESTBIN)

demo: $(TARGET)
	$(TARGET) --batch examples/route-set.txt --user admin --password Admin#2024

clean:
	@$(call RM,$(BUILD))

help:
	@echo "RailControl targets:"
	@echo "  all    build $(TARGET)"
	@echo "  run    interactive control console"
	@echo "  sim    60 second headless simulation"
	@echo "  demo   run the batch example"
	@echo "  test   build and run the unit tests"
	@echo "  clean  remove $(BUILD)/"
