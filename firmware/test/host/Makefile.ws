# =============================================================================
# Makefile.ws — host-side WebServer production REST test
# =============================================================================
# Compiles the REAL RelayHandlers.h (header-only, included via WebServerTest.cpp)
# + TransactionJournal.cpp + JournalRecord.cpp + MqttClient.cpp (for the
# shims + Utils::computeCommandHash) against comprehensive shims.
#
# Test calls actual Web::Handlers::handleRelay() with JSON body via WebServer shim.
#
# Usage:
#     make -f Makefile.ws           # build
#     make -f Makefile.ws run       # build + run
#     make -f Makefile.ws clean     # clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
SUPPRESS = -Wno-unused-parameter -Wno-array-bounds -Wno-stringop-truncation -Wno-unused-variable

ROOT_DIR := $(CURDIR)
SRC_DIR  := $(ROOT_DIR)/../..
SHIM_DIR := $(ROOT_DIR)/shims
JSON_DIR := $(SRC_DIR)/.pio/libdeps/development/ArduinoJson/src

TEST_SRC  := $(ROOT_DIR)/WebServerTest.cpp
FIRMWARE1 := $(SRC_DIR)/MqttClient.cpp
FIRMWARE2 := $(SRC_DIR)/TransactionJournal.cpp
FIRMWARE3 := $(SRC_DIR)/JournalRecord.cpp

BIN      := web_server_test_bin

.PHONY: all run clean

all: $(BIN)

$(BIN): $(TEST_SRC) $(FIRMWARE1) $(FIRMWARE2) $(FIRMWARE3) \
        $(SHIM_DIR)/MqttClientDeps.h $(SHIM_DIR)/Arduino.h $(SHIM_DIR)/Preferences.h \
        $(SHIM_DIR)/esp_crc.h $(SHIM_DIR)/Config.h \
        $(SRC_DIR)/RestJournalHelper.h $(SRC_DIR)/Common.h $(SRC_DIR)/RelayHandlers.h \
        $(SRC_DIR)/CommandHash.h
	$(CXX) $(CXXFLAGS) $(SUPPRESS) \
	    -I$(SHIM_DIR) \
	    -I$(SRC_DIR) \
	    -I$(JSON_DIR) \
	    -include $(SHIM_DIR)/MqttClientDeps.h \
	    $(TEST_SRC) $(FIRMWARE1) $(FIRMWARE2) $(FIRMWARE3) \
	    -lcrypto \
	    -o $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
