# =============================================================================
# Makefile.mc — host-side MqttClient production-path test
# =============================================================================
# Compiles the REAL MqttClient.cpp + TransactionJournal.cpp + JournalRecord.cpp
# against comprehensive shims. Test calls actual _handleCommand() and _handleOta().
#
# Usage:
#     make -f Makefile.mc           # build
#     make -f Makefile.mc run       # build + run
#     make -f Makefile.mc clean     # clean
# =============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror

# Suppress false positives from production code that can't be fixed without
# modifying production source (unused params, array-bounds on validated idx)
SUPPRESS = -Wno-unused-parameter -Wno-array-bounds -Wno-stringop-truncation -Wno-unused-variable

ROOT_DIR := $(CURDIR)
SRC_DIR  := $(ROOT_DIR)/../..
SHIM_DIR := $(ROOT_DIR)/shims
JSON_DIR := $(SRC_DIR)/.pio/libdeps/development/ArduinoJson/src

TEST_SRC  := $(ROOT_DIR)/MqttClientTest.cpp
FIRMWARE1 := $(SRC_DIR)/MqttClient.cpp
FIRMWARE2 := $(SRC_DIR)/TransactionJournal.cpp
FIRMWARE3 := $(SRC_DIR)/JournalRecord.cpp

BIN      := mqtt_client_test_bin

.PHONY: all run clean

all: $(BIN)

$(BIN): $(TEST_SRC) $(FIRMWARE1) $(FIRMWARE2) $(FIRMWARE3) \
       $(SHIM_DIR)/MqttClientDeps.h $(SHIM_DIR)/Arduino.h $(SHIM_DIR)/Preferences.h \
       $(SHIM_DIR)/esp_crc.h $(SHIM_DIR)/Config.h
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
