# ==============================================================================
# VFRS - Virtual Frame Relay Switch
# Modernized Makefile for MSYS2 UCRT64 (GCC)
# ==============================================================================

# Toolchain configuration
CC       ?= gcc
AR       ?= ar
RM        = rm -rf
MKDIR_P  ?= mkdir -p

# Build configuration: BUILD=release (default) or BUILD=debug
BUILD    ?= release

# Base Compiler and Linker Flags
BASE_CFLAGS  = -Wall -Wextra -Wno-unused-parameter \
               -D_WIN32_WINNT=0x0A00 -D_GNU_SOURCE \
               -Iinclude -Isrc

LDFLAGS_BASE = -static -lws2_32 -lwinmm -ladvapi32

ifeq ($(BUILD),debug)
    OPT_FLAGS     = -O0 -g3 -DDEBUG -D_DEBUG -fno-omit-frame-pointer
    LDFLAGS_EXTRA =
else
    OPT_FLAGS     = -O2 -DNDEBUG
    LDFLAGS_EXTRA = -s
endif

CFLAGS   = $(OPT_FLAGS) $(BASE_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS  = $(LDFLAGS_BASE) $(LDFLAGS_EXTRA) $(EXTRA_LDFLAGS)

# Directory Structure
SRC_DIR   = src
INC_DIR   = include
BUILD_DIR = build
BIN_DIR   = bin
TESTS_DIR = tests

# Target Artifacts
TARGET     = $(BIN_DIR)/vfrs.exe
STATIC_LIB = $(BIN_DIR)/libvfrs.a
IE_TEST    = $(BIN_DIR)/tests/ie_test.exe
CFG_TEST   = $(BIN_DIR)/tests/cfg_test.exe

# ------------------------------------------------------------------------------
# Source File Structure
# ------------------------------------------------------------------------------

# Core & Infrastructure
SRCS_CORE = $(SRC_DIR)/core/logger.c \
            $(SRC_DIR)/core/cfg_lexer.c \
            $(SRC_DIR)/core/cfg_parser.c \
            $(SRC_DIR)/core/cfg_schema.c \
            $(SRC_DIR)/core/cfg_compiler.c \
            $(SRC_DIR)/core/config.c

# Frame Relay Switching
SRCS_SWITCH = $(SRC_DIR)/switching/fr_frame.c \
              $(SRC_DIR)/switching/fr_fragment.c \
              $(SRC_DIR)/switching/fr_switch.c \
              $(SRC_DIR)/switching/svc_routing_common.c

# Ports & Interface Drivers
SRCS_PORTS = $(SRC_DIR)/ports/port_common.c \
             $(SRC_DIR)/ports/port_queue.c \
             $(SRC_DIR)/ports/port_udp.c \
             $(SRC_DIR)/ports/port_tcp.c \
             $(SRC_DIR)/ports/port_serial.c \
             $(SRC_DIR)/ports/port_pipe.c \
             $(SRC_DIR)/ports/lapf/port_lapf.c \
             $(SRC_DIR)/ports/pcap/port_pcap.c \
             $(SRC_DIR)/ports/svc_numbering/svc_numbering.c

# PVC, LMI & Multicast
SRCS_PVC = $(SRC_DIR)/pvc/pvc_lmi_common.c \
           $(SRC_DIR)/pvc/pvc_lmi_ansi.c \
           $(SRC_DIR)/pvc/pvc_lmi_gof.c \
           $(SRC_DIR)/pvc/pvc_lmi_q933a.c \
           $(SRC_DIR)/pvc/pvc_mcast_uni.c \
           $(SRC_DIR)/pvc/pvc_mcast_nni.c

# Congestion Management & CLLM
SRCS_CGST = $(SRC_DIR)/congestion/cgst_mgnt.c \
            $(SRC_DIR)/congestion/cgst_cllm.c

# SVC Signalling (Q.933 / X.36 / X.76) & SPVC
SRCS_SVC = $(SRC_DIR)/svc/svc_sig_common.c \
           $(SRC_DIR)/svc/svc_sig_iel.c \
           $(SRC_DIR)/svc/svc_sig_iep.c \
           $(SRC_DIR)/svc/svc_sig_uni.c \
           $(SRC_DIR)/svc/svc_sig_nni.c \
           $(SRC_DIR)/svc/svc_spvc.c

# Library sources (all except main.c)
LIB_SRCS  = $(SRCS_CORE) $(SRCS_SWITCH) $(SRCS_PORTS) $(SRCS_PVC) $(SRCS_CGST) $(SRCS_SVC)
MAIN_SRC  = $(SRC_DIR)/main.c
ALL_SRCS  = $(MAIN_SRC) $(LIB_SRCS)

# Object files mapping (build/src/...)
LIB_OBJS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/src/%.o,$(LIB_SRCS))
MAIN_OBJ  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/src/%.o,$(MAIN_SRC))
ALL_OBJS  = $(MAIN_OBJ) $(LIB_OBJS)

# Dependency files (.d)
DEPS      = $(ALL_OBJS:.o=.d) $(BUILD_DIR)/tests/ie_test.d

# ------------------------------------------------------------------------------
# Primary Build Targets
# ------------------------------------------------------------------------------

.PHONY: all debug release static-lib test check clean distclean help dirs

all: $(TARGET)

debug:
	@$(MAKE) BUILD=debug all

release:
	@$(MAKE) BUILD=release all

# Monolithic Executable Target
$(TARGET): $(ALL_OBJS)
	@$(MKDIR_P) $(BIN_DIR)
	@echo "  [LD]  $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Static Library Target (Modular Switch Core)
static-lib: $(STATIC_LIB)

$(STATIC_LIB): $(LIB_OBJS)
	@$(MKDIR_P) $(BIN_DIR)
	@echo "  [AR]  $@"
	@$(AR) rcs $@ $^

# Compilation rule for src/
$(BUILD_DIR)/src/%.o: $(SRC_DIR)/%.c
	@$(MKDIR_P) $(dir $@)
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Compilation rule for tests/
$(BUILD_DIR)/tests/%.o: $(TESTS_DIR)/%.c
	@$(MKDIR_P) $(dir $@)
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ------------------------------------------------------------------------------
# Testing Targets
# ------------------------------------------------------------------------------

# C Unit Test binary for Q.933 IEs
IE_TEST_OBJS = $(BUILD_DIR)/tests/ie_test.o \
               $(BUILD_DIR)/src/switching/fr_frame.o \
               $(BUILD_DIR)/src/congestion/cgst_cllm.o \
               $(BUILD_DIR)/src/svc/svc_sig_iel.o \
               $(BUILD_DIR)/src/svc/svc_sig_iep.o

$(IE_TEST): $(IE_TEST_OBJS)
	@$(MKDIR_P) $(BIN_DIR)/tests
	@echo "  [LD]  $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# C Unit Test binary for Configuration Parser & Numbering Engine
CFG_TEST_OBJS = $(BUILD_DIR)/tests/cfg_test.o \
                $(LIB_OBJS)

$(CFG_TEST): $(CFG_TEST_OBJS)
	@$(MKDIR_P) $(BIN_DIR)/tests
	@echo "  [LD]  $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Run full test suite (C unit tests + Python compliance & functional tests)
test: all $(IE_TEST) $(CFG_TEST)
	@echo "=== Running C Unit Tests (IE Parser/Builder) ==="
	@./$(IE_TEST)
	@echo "=== Running C Unit Tests (Configuration Parser & Digit Trie) ==="
	@./$(CFG_TEST)
	@echo "=== Running Python SVC Compliance Test Suite ==="
	@python tests/svc_compliance_test.py
	@echo "=== Running Python SVC Functional Test Suite ==="
	@python tests/svc_test.py
	@echo "=== All Tests Passed Successfully ==="

# Run pipe loopback integration check
check: debug
	@echo "=== Running Pipe Loopback Self-Test ==="
	@bash ./tests/run_pipe_loopback_test.sh

# ------------------------------------------------------------------------------
# Housekeeping Targets
# ------------------------------------------------------------------------------

clean:
	@echo "  [CLEAN]"
	@$(RM) $(BUILD_DIR) $(BIN_DIR)
	@$(RM) tests/loopback.conf tests/svc_compliance.conf tests/svc_test.conf tests/*.log tests/*.pcap

distclean: clean
	@$(RM) *.log *.pcap *.exe

help:
	@echo "VFRS Build System"
	@echo "================="
	@echo "Targets:"
	@echo "  all         : Build release executable ($(TARGET))"
	@echo "  debug       : Build with debug symbols and assertions (-O0 -g3)"
	@echo "  release     : Build optimized release binary (-O2 -DNDEBUG)"
	@echo "  static-lib  : Build monolithic static library ($(STATIC_LIB))"
	@echo "  test        : Build and run all C unit & Python test suites"
	@echo "  check       : Build debug binary and run loopback smoke test"
	@echo "  clean       : Remove build/ and bin/ directories"
	@echo "  distclean   : Remove all artifacts, temporary configs, and logs"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD=release|debug   (default: release)"
	@echo "  CC=<compiler>         (default: gcc)"
	@echo "  EXTRA_CFLAGS=<flags>  (additional C compiler flags)"
	@echo "  EXTRA_LDFLAGS=<flags> (additional linker flags)"

# Auto-include generated dependency files (.d)
-include $(DEPS)