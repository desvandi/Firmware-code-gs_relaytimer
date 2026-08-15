# =============================================================================
# Makefile.mc — host-side MqttClient production-path test
# =============================================================================
# Compiles the REAL MqttClient.cpp + TransactionJournal.cpp + JournalRecord.cpp
# against comprehensive shims. Test calls actual _handleCommand() and _handleOta().
#
# Usage:
#     ./setup_host_env.sh          # one-time: install ArduinoJson + compat header
#     make -f Makefile.mc           # build
#     make -f Makefile.mc run       # build + run
#     make -f Makefile.mc clean     # clean
#
# C3-GATE-002 TEST-INFRA: requires ArduinoJson v6.18.2 (NOT v6.19+) +
# shims/arduinojson_compat.h. Run ./setup_host_env.sh once before first build.
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
       $(SHIM_DIR)/esp_crc.h $(SHIM_DIR)/Config.h $(SHIM_DIR)/arduinojson_compat.h
	$(CXX) $(CXXFLAGS) $(SUPPRESS) \
	    -I$(SHIM_DIR) \
	    -I$(SRC_DIR) \
	    -I$(JSON_DIR) \
	    -include $(SHIM_DIR)/arduinojson_compat.h \
	    -include $(SHIM_DIR)/MqttClientDeps.h \
	    $(TEST_SRC) $(FIRMWARE1) $(FIRMWARE2) $(FIRMWARE3) \
	    -lcrypto \
	    -o $(BIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
