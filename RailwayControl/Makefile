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
OUTDIR  := output
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
    CP       := copy /y
    LDLIBS   := -lwinmm -lws2_32
    CFLAGS   += -D_WIN32_WINNT=0x0601
else
    EXE      :=
    MKDIR    := mkdir -p $(1)
    MKDIRP   := mkdir -p $(1)
    CP       := cp -f
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
#
# Kept in step with what is on disk: a source listed here but absent fails the
# build with a confusing "No rule to make target" message.
# ---------------------------------------------------------------------------
CORE_SRC := \
    $(SRCDIR)/main.c \
    $(SRCDIR)/railway_api.c \
    $(SRCDIR)/rc_account.c \
    $(SRCDIR)/rc_audio.c \
    $(SRCDIR)/rc_settings.c \
    $(SRCDIR)/rc_version.c \
    $(SRCDIR)/rcp_car.c \
    $(SRCDIR)/rcp_emergency.c \
    $(SRCDIR)/rcp_eventlog.c \
    $(SRCDIR)/rcp_interlock.c \
    $(SRCDIR)/rcp_layout.c \
    $(SRCDIR)/rcp_names.c \
    $(SRCDIR)/naming.c \
    $(SRCDIR)/rcp_point.c \
    $(SRCDIR)/rcp_sensor.c \
    $(SRCDIR)/rcp_signal.c \
    $(SRCDIR)/rcp_track.c \
    $(SRCDIR)/rcp_train.c \
    $(SRCDIR)/rcp_util.c \
    $(SRCDIR)/conjob.c \
    $(SRCDIR)/terminal.c \
    $(SRCDIR)/mcu_plugin.c

# Test sources. The suite is not present yet; the target below degrades to a
# clear message rather than a make error when tests/ is absent.
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
GUITARGET := $(BUILD)/rcp-gui$(EXE)
INCLUDES := -I$(INCDIR)

# ---------------------------------------------------------------------------
# GUI (Win32 dispatch console)
#
# A separate executable from the console build. It shares every engine object
# and adds the window, so there is one implementation of the railway and two
# front ends over it. Only meaningful on Windows.
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
GUI_SRC  := \
    $(SRCDIR)/win32_entry.c \
    $(SRCDIR)/win32_main.c \
    $(SRCDIR)/win32_panel.c \
    $(SRCDIR)/win32_status.c \
    $(SRCDIR)/win32_terminal.c \
    $(SRCDIR)/win32_actions.c

GUI_OBJ  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(GUI_SRC))

# -mwindows sets the WINDOWS subsystem so no console window appears, and
# -municode selects the wWinMain entry point.
GUI_LDFLAGS := $(LDFLAGS) -mwindows -municode
GUI_LDLIBS  := $(LDLIBS) -lgdi32 -luser32 -lcomdlg32 -lshell32

# Note: main.o is deliberately excluded - the GUI has its own entry point in
# win32_entry.c, and linking both would give two WinMain/main definitions.
$(GUITARGET): $(GUI_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(GUI_OBJ) $(LIB_OBJ) -o $@ $(GUI_LDFLAGS) $(GUI_LDLIBS)

.PHONY: gui

gui: $(GUITARGET)
	@echo "Built $(GUITARGET)"

else

.PHONY: gui

gui:
	@echo "The GUI is Win32 only; build targets on this platform are rcp and rcp-tests."

endif

# ---------------------------------------------------------------------------
# Tools
#
# Small diagnostics that link the engine but are not part of the product.
# They exist so a layout or policy change can be checked before it ships.
# ---------------------------------------------------------------------------
NAMESTOOL := $(BUILD)/names_tool$(EXE)
AUTHTOOL  := $(BUILD)/auth_console$(EXE)

$(NAMESTOOL): tools/names_tool.c $(LIB_OBJ)
	@$(call MKDIRP,$(BUILD))
	$(CC) $(CFLAGS) $(INCLUDES) tools/names_tool.c $(LIB_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(AUTHTOOL): tools/auth_console.c $(LIB_OBJ)
	@$(call MKDIRP,$(BUILD))
	$(CC) $(CFLAGS) $(INCLUDES) tools/auth_console.c $(LIB_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

.PHONY: namestool authtool tools

namestool: $(NAMESTOOL)
	@echo "Built $(NAMESTOOL)"
	$(NAMESTOOL)

authtool: $(AUTHTOOL)
	@echo "Built $(AUTHTOOL)"
	$(AUTHTOOL) --accounts

tools: $(NAMESTOOL) $(AUTHTOOL)
	@echo "Built $(NAMESTOOL) and $(AUTHTOOL)"

# ---------------------------------------------------------------------------
# Distribution
#
# Build every front end and copy the finished binaries into output/, which is
# the folder you hand to somebody. Objects stay in build/ - nothing
# intermediate is left in output/.
# ---------------------------------------------------------------------------
.PHONY: dist dist-console dist-gui dist-tools

dist: $(TARGET) $(GUITARGET) $(NAMESTOOL) $(AUTHTOOL)
	@$(call MKDIRP,$(OUTDIR))
	@echo "Copying binaries into $(OUTDIR)/ ..."
	$(CP) $(TARGET)    $(OUTDIR)/
	$(CP) $(GUITARGET) $(OUTDIR)/
	$(CP) $(NAMESTOOL) $(OUTDIR)/
	$(CP) $(AUTHTOOL)  $(OUTDIR)/
	@echo ""
	@echo "$(OUTDIR)/ contains:"
	@echo "  $(notdir $(TARGET))     - signaller console"
	@echo "  $(notdir $(GUITARGET)) - Win32 dispatch console"
	@echo "  $(notdir $(NAMESTOOL)) - naming diagnostics"
	@echo "  $(notdir $(AUTHTOOL))  - account console"

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

ifeq ($(wildcard $(TESTDIR)),)
# The unit-test suite is not present in this tree. Say so plainly rather than
# letting make fail with "No rule to make target 'tests/test_main.c'".
.PHONY: test

test:
	@echo "The unit-test suite is not present in this tree (no $(TESTDIR)/ directory)."
	@echo "Build and run the tools instead:  make namestool"
else

test: $(TESTBIN)
	$(TESTBIN)

endif

demo: $(TARGET)
	$(TARGET) --batch examples/route-set.txt --user admin --password Admin#2024

clean:
	@$(call RM,$(BUILD))

help:
	@echo "RailControl targets:"
	@echo "  all       build $(TARGET)"
	@echo "  gui       build $(GUITARGET) (Win32 dispatch console)"
	@echo "  dist      build everything and copy the binaries into $(OUTDIR)/"
	@echo "  run       interactive control console"
	@echo "  sim       60 second headless simulation"
	@echo "  demo      run the batch example"
	@echo "  test      build and run the unit tests"
	@echo "  namestool build and run the naming diagnostics"
	@echo "  authtool  build and run the account console"
	@echo "  clean     remove $(BUILD)/"
