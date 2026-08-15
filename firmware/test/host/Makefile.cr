# =============================================================================
# Makefile.cr — host-side P2-2 F-P0-1 command-routing test harness
# =============================================================================
# Builds the command-routing test binary by compiling:
#   - firmware/test/host/CommandRoutingTest.cpp  (the routing test)
#   - firmware/TransactionJournal.cpp            (P2-1 Rev26 implementation — REAL)
#   - firmware/JournalRecord.cpp                 (Phase 1 foundation — REAL)
# against the existing shims:
#   - firmware/test/host/shims/Arduino.h
#   - firmware/test/host/shims/Preferences.h
#   - firmware/test/host/shims/Config.h
#   - firmware/test/host/shims/esp_crc.h
#
# The routing test replicates MqttClient::_handleCommand() routing decision
# (type switch + CommitMode selection) and calls the REAL TransactionJournal
# API. No mocking of the unit under test — the firmware source files compiled
# here are the SAME ones that get flashed to the ESP32.
#
# Usage:
#     make -f Makefile.cr           # build the test binary
#     make -f Makefile.cr run        # build + run
#     make -f Makefile.cr clean      # remove build artifacts
# =============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror

ROOT_DIR := $(CURDIR)
SRC_DIR  := $(ROOT_DIR)/../..
SHIM_DIR := $(ROOT_DIR)/shims

TEST_SRC  := $(ROOT_DIR)/CommandRoutingTest.cpp
FIRMWARE1 := $(SRC_DIR)/TransactionJournal.cpp
FIRMWARE2 := $(SRC_DIR)/JournalRecord.cpp

BIN       := command_routing_test_bin

.PHONY: all run clean

all: $(BIN)

$(BIN): $(TEST_SRC) $(FIRMWARE1) $(FIRMWARE2) \
        $(SHIM_DIR)/Arduino.h $(SHIM_DIR)/Preferences.h \
        $(SHIM_DIR)/Config.h $(SHIM_DIR)/esp_crc.h
	$(CXX) $(CXXFLAGS) \
	    -I$(SHIM_DIR) \
	    -I$(SRC_DIR) \
	    $(TEST_SRC) $(FIRMWARE1) $(FIRMWARE2) \
	    -o $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
