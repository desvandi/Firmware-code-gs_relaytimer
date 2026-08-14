# =============================================================================
# Makefile — host-side Phase 1 test harness for JournalRecord
# =============================================================================
# Builds the test binary by compiling the REAL firmware/JournalRecord.cpp
# against two minimal shims (Arduino.h, esp_crc.h). No mocking of the unit
# under test — the source file is the same one that gets flashed to the ESP32.
#
# Usage:
#     make           # build the test binary
#     make run       # build + run
#     make clean     # remove build artifacts
# =============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror

ROOT_DIR := $(CURDIR)
SRC_DIR  := $(ROOT_DIR)/../..
SHIM_DIR := $(ROOT_DIR)/shims

TEST_SRC := $(ROOT_DIR)/JournalRecordTest.cpp
FIRMWARE := $(SRC_DIR)/JournalRecord.cpp

BIN      := $(ROOT_DIR)/journal_record_test

.PHONY: all run clean

all: $(BIN)

$(BIN): $(TEST_SRC) $(FIRMWARE) $(SHIM_DIR)/Arduino.h $(SHIM_DIR)/esp_crc.h
	$(CXX) $(CXXFLAGS) \
	    -I$(SHIM_DIR) \
	    -I$(SRC_DIR) \
	    $(TEST_SRC) $(FIRMWARE) \
	    -o $(BIN)

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
