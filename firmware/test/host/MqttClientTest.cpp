// =============================================================================
// MqttClientTest.cpp — Production command-path behavioral proof (F-P0-1-C4)
// =============================================================================
// This test calls the REAL MqttClient::_handleCommand() and _handleOta()
// methods with actual JSON payloads. It does NOT replicate routing logic.
//
// Uses #define private public to access private methods.
// Uses MqttClientDeps.h shim for ESP32 dependencies.
// Uses REAL TransactionJournal (compiled from same source as ESP32).
// =============================================================================
#define private public
#include "MqttClient.h"
#undef private

#include "MqttClientDeps.h"
#include "TransactionJournal.h"  // Need journal extern declaration
#include "JournalRecord.h"  // Need JournalRecord type  // Must come AFTER MqttClient.h to avoid double-include

#include <cstdio>
#include <cstring>
#include <signal.h>
#include <setjmp.h>

using namespace Services;

// --- Test framework ---
static int g_passCount = 0;
static int g_failCount = 0;

#define CHECK(cond, msg) do { \
  if (cond) { printf("  [PASS] %s\n", msg); g_passCount++; } \
  else { printf("  [FAIL] %s (line %d)\n", msg, __LINE__); g_failCount++; } \
} while(0)

#define CHECK_EQ(actual, expected, msg) do { \
  auto _a = (actual); auto _e = (expected); \
  if (_a == _e) { printf("  [PASS] %s\n", msg); g_passCount++; } \
  else { printf("  [FAIL] %s (got=%d, expected=%d, line %d)\n", msg, (int)_a, (int)_e, __LINE__); g_failCount++; } \
} while(0)

// Access the global journal (declared in TransactionJournal.h as extern TransactionJournal journal;)
// It's defined in TransactionJournal.cpp within namespace Services.
using Services::journal;

// --- Reset helpers ---
static void resetNVS() { Preferences::clearAllStorage(); }
static void resetEnv() {
  resetNVS();
  // Reset Core globals
  for (int i = 0; i < 12; i++) {  // NUM_CHANNELS = 12
    Core::relayState[i] = false;
    Core::relaySource[i] = Core::RelaySource::Manual;
    Core::channels[i].schedCount = 0;
    strncpy(Core::channels[i].name, "", 1);
  }
  host_shim::g_espRestartCalled = false;
  // Reconstruct journal
  journal.~TransactionJournal();
  new (&journal) TransactionJournal();
  journal.begin();
  journal.setBootPhase(Services::BootPhase::RUNNING);  // Required for _handleCommand to accept commands
}

// Dummy MQTT callback for MqttClient construction
static MqttClient* g_mqttPtr = nullptr;

int main() {
  printf("==========================================================\n");
  printf("MqttClient Production Command-Path Test (F-P0-1-C4)\n");
  printf("Calls REAL _handleCommand() and _handleOta()\n");
  printf("==========================================================\n");

  // Create MqttClient instance (uses shims for all ESP32 deps)
  MqttClient mqtt;
  g_mqttPtr = &mqtt;
  // Initialize journal
  resetEnv();

  // ---- TEST 1: Schedule upsert via _handleCommand ----
  printf("\n[TEST 1] Schedule upsert via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"schedule","action":"upsert","requestId":"req-sched-1","channelId":1,"id":0,"onTime":"07:00","offTime":"18:00","dayMask":127,"enabled":true})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-sched-1"),
          "schedule entry is COMMITTED (not DURABILITY_FAILURE)");
    CHECK(journal.getTransactionState("req-sched-1") == TransactionState::COMMITTED,
          "schedule state is COMMITTED");
  }

  // ---- TEST 2: PIR config via _handleCommand ----
  printf("\n[TEST 2] PIR config via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"pir","action":"config","requestId":"req-pir-1","id":1,"enabled":true,"holdTime":120})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-pir-1"),
          "PIR entry is COMMITTED (not DURABILITY_FAILURE)");
  }

  // ---- TEST 3: Channel rename via _handleCommand ----
  printf("\n[TEST 3] Channel rename via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"channel","action":"rename","requestId":"req-chan-1","channelId":1,"name":"Kitchen"})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-chan-1"),
          "channel entry is COMMITTED (not DURABILITY_FAILURE)");
  }

  // ---- TEST 4: Time set via _handleCommand ----
  printf("\n[TEST 4] Time set via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"time","action":"set","requestId":"req-time-1","datetime":"2024-01-15T10:30:00"})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-time-1"),
          "time entry is COMMITTED (not DURABILITY_FAILURE)");
  }

  // ---- TEST 5: System resetEnergyStats via _handleCommand ----
  printf("\n[TEST 5] System resetEnergyStats via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"system","action":"resetEnergyStats","requestId":"req-reset-1"})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-reset-1"),
          "system reset entry is COMMITTED (not DURABILITY_FAILURE)");
  }

  // ---- TEST 6: Config setDevice via _handleCommand ----
  printf("\n[TEST 6] Config setDevice via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"config","action":"setDevice","requestId":"req-conf-1","deviceName":"TestDevice","timezone":7})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-conf-1"),
          "config entry is COMMITTED (not DURABILITY_FAILURE)");
  }

  // ---- TEST 7: getStatus — NO journal entry ----
  printf("\n[TEST 7] getStatus via _handleCommand() — NO journal entry\n");
  {
    resetEnv();
    uint8_t sizeBefore = journal.getJournalSize();
    const char* json = R"({"type":"system","action":"getStatus","requestId":"req-status-1"})";
    mqtt._handleCommand(json);
    uint8_t sizeAfter = journal.getJournalSize();
    CHECK(sizeAfter == sizeBefore,
          "journal size unchanged by getStatus (no entry created)");
    CHECK(!journal.isProcessed("req-status-1"),
          "getStatus requestId NOT in journal (CommitMode::NONE works)");
  }

  // ---- TEST 8: Relay ON via _handleCommand ----
  printf("\n[TEST 8] Relay ON via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"relay","action":"on","requestId":"req-relay-1","channelId":1})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-relay-1"),
          "relay entry is COMMITTED (EXECUTING → COMMITTED path works)");
  }

  // ---- TEST 9: Reboot via _handleCommand ----
  printf("\n[TEST 9] Reboot via _handleCommand()\n");
  {
    resetEnv();
    const char* json = R"({"type":"system","action":"reboot","requestId":"req-reboot-1"})";
    mqtt._handleCommand(json);
    CHECK(journal.isCommitted("req-reboot-1"),
          "reboot entry is COMMITTED before restart");
    CHECK(journal.getPendingAckCount() >= 1,
          "reboot ACK in queue (not dequeued — durable evidence)");
    CHECK(host_shim::g_espRestartCalled,
          "ESP.restart() was called (after durable commit)");
  }

  // ---- TEST 10: OTA via _handleOta (failure path — no HTTPS server) ----
  printf("\n[TEST 10] OTA via _handleOta() (failure path)\n");
  {
    resetEnv();
    const char* json = R"({"action":"update","requestId":"req-ota-1","url":"https://example.com/fw.bin","version":"4.1.0","size":100000,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","signature":"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"})";
    mqtt._handleOta(json);
    // OTA download will fail (no real HTTPS server) — verify markExecuting was called
    CHECK(journal.getTransactionState("req-ota-1") == TransactionState::EXECUTING ||
          journal.getTransactionState("req-ota-1") == TransactionState::COMMITTED,
          "OTA entry exists (EXECUTING or COMMITTED — markExecuting was called by production code)");
    CHECK(journal.isProcessed("req-ota-1"),
          "OTA requestId is in journal (production _handleOta created entry)");
  }

  // ---- TEST 11: Non-relay commands don't fill journal with PENDING ----
  printf("\n[TEST 11] Journal not exhausted by non-relay commands\n");
  {
    resetEnv();
    // Send 5 schedule commands — previously each would leave a PENDING entry
    for (int i = 0; i < 5; i++) {
      char json[256];
      // Use different channels (1-5) to avoid schedule limit (max 4 per channel)
      snprintf(json, sizeof(json),
        R"({"type":"schedule","action":"upsert","requestId":"req-multi-%d","channelId":%d,"id":0,"onTime":"07:00","offTime":"18:00","dayMask":127,"enabled":true})", i, i+1);
      mqtt._handleCommand(json);
    }
    // All 5 should be COMMITTED (not PENDING/DURABILITY_FAILURE)
    int committedCount = 0;
    for (int i = 0; i < 5; i++) {
      char rid[32];
      snprintf(rid, sizeof(rid), "req-multi-%d", i);
      if (journal.isCommitted(rid)) committedCount++;
    }
    CHECK_EQ(committedCount, 5, "all 5 schedule commands COMMITTED (no DURABILITY_FAILURE)");
  }

  // ---- TEST 12: Duplicate requestId replays ACK ----
  printf("\n[TEST 12] Duplicate requestId replays ACK\n");
  {
    resetEnv();
    const char* json1 = R"({"type":"relay","action":"on","requestId":"req-dup-1","channelId":1})";
    mqtt._handleCommand(json1);
    CHECK(journal.isCommitted("req-dup-1"), "first relay command committed");

    // Send same requestId again — should replay, not re-execute
    const char* json2 = R"({"type":"relay","action":"on","requestId":"req-dup-1","channelId":1})";
    mqtt._handleCommand(json2);
    CHECK(journal.isCommitted("req-dup-1"), "duplicate still committed");
    // Journal size should not have grown
    CHECK_EQ(journal.getJournalSize(), (uint8_t)1, "journal size unchanged (no new slot for duplicate)");
  }

  // ---- TEST 13: Validation failure after storeIntent → clearEntry ----
  printf("\n[TEST 13] Validation failure after storeIntent → clearEntry\n");
  {
    resetEnv();
    // Schedule with invalid onTime format — should fail AFTER storeIntent
    const char* json = R"({"type":"schedule","action":"upsert","requestId":"req-fail-1","channelId":1,"id":0,"onTime":"invalid","offTime":"18:00","dayMask":127,"enabled":true})";
    uint8_t sizeBefore = journal.getJournalSize();
    mqtt._handleCommand(json);
    uint8_t sizeAfter = journal.getJournalSize();
    CHECK(!journal.isProcessed("req-fail-1"),
          "failed schedule entry cleared from journal (clearEntry called)");
    CHECK_EQ(sizeAfter, sizeBefore, "journal size unchanged (slot was cleared)");
  }

  printf("\n==========================================================\n");
  printf("RESULTS: %d passed, %d failed\n", g_passCount, g_failCount);
  printf("==========================================================\n");
  return (g_failCount == 0) ? 0 : 1;
}
