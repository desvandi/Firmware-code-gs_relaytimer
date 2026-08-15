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

  // ---- TEST 10: OTA via _handleOta() — PENDING → EXECUTING behavioral proof ----
  //
  // AUDITOR REQUIREMENT (F-P0-1-C4 → C5): the previous TEST 10 asserted
  //   `EXECUTING || COMMITTED`, which is too permissive — a COMMITTED state
  //   would also satisfy the assertion without proving markExecuting() was
  //   called. The auditor requires strict behavioral proof that captures the
  //   PENDING → EXECUTING transition.
  //
  // CORRECTED DESIGN — Option C (auditor's "cleanest proof"):
  //   Failure injection INSIDE _downloadAndVerifyOta() — production code
  //   reaches the download function, fails at the root CA check, and
  //   returns false. State stays EXECUTING (commitTransaction is only
  //   called on success).
  //
  //   PRE-CONDITION : requestId not in journal (no entry)
  //   FAILURE POINT : OTA_HTTPS_ROOT_CA is "" (shim default) → line 1923
  //                   inside _downloadAndVerifyOta() returns false
  //   POST-CONDITION: state is EXACTLY EXECUTING (strict equality)
  //
  // Production path exercised (MqttClient.cpp):
  //   line 1586  storeIntent()           → PENDING  (gen=N)
  //   line 1595  markExecuting()          → EXECUTING (gen=N+1)
  //   line 1603  action == "update"      ✓
  //   line 1617  url not empty + https   ✓
  //   line 1622  expectedSize 1..2MB     ✓
  //   line 1627  expectedSha256 len == 64 ✓
  //   line 1653  version X.Y.Z format    ✓
  //   line 1682  version > current        ✓ (4.1.0 > 4.0.0)
  //   line 1695  ED25519_PUBLIC_KEY set   ✓ (set to "00" by test)
  //   line 1704  signatureHex len == 128 ✓ (128 chars)
  //   line 1711  url starts with https:// ✓
  //   line 1729  host allowlist          — skipped (OTA_ALLOWED_HOSTS empty)
  //   line 1797  _downloadAndVerifyOta() → enters function
  //   line 1923  OTA_HTTPS_ROOT_CA == "" → return false ← INJECTION POINT
  //   line 1800  success == false         → publish fail ACK
  //   line 1812  commitTransaction()    ✗ NOT REACHED
  //
  // Strict equality with EXECUTING proves:
  //   (a) storeIntent was called (otherwise isProcessed would be false)
  //   (b) markExecuting was called (otherwise state would be PENDING)
  //   (c) commitTransaction was NOT called (otherwise state would be COMMITTED)
  printf("\n[TEST 10] OTA via _handleOta() — PENDING → EXECUTING behavioral proof\n");
  {
    resetEnv();

    // ARM failure injection:
    //   - OTA_ED25519_PUBLIC_KEY_HEX must be NON-EMPTY so production validation
    //     at line 1695 passes and we actually reach the download function.
    //   - OTA_HTTPS_ROOT_CA stays empty (shim default) → triggers failure
    //     INSIDE _downloadAndVerifyOta() at line 1923.
    Core::OTA_ED25519_PUBLIC_KEY_HEX = "00";  // any non-empty string
    // OTA_HTTPS_ROOT_CA already "" by shim default — failure injection armed

    // PRE-CONDITION 1: requestId not in journal (no PENDING entry exists)
    CHECK(!journal.isProcessed("req-ota-1"),
          "PRE: requestId 'req-ota-1' not in journal — no PENDING entry exists");

    // PRE-CONDITION 2: getTransactionState returns PENDING for unknown requestId
    // (this is the sentinel value returned by _findSlot miss — see line 1531)
    CHECK(journal.getTransactionState("req-ota-1") == TransactionState::PENDING,
          "PRE: getTransactionState returns PENDING for unknown requestId (sentinel)");

    // Valid OTA JSON — passes ALL pre-download validations.
    // Signature is exactly 128 hex chars (64 bytes * 2) — passes line 1704.
    // Signature VALUE is bogus, but signature verification only runs AFTER
    // download succeeds (line 2064), so it's irrelevant for this failure path.
    const char* json = R"({"action":"update","requestId":"req-ota-1","url":"https://example.com/fw.bin","version":"4.1.0","size":100000,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","signature":"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"})";

    // Execute PRODUCTION _handleOta() — calls real storeIntent/markExecuting
    // against real TransactionJournal compiled from production source.
    mqtt._handleOta(json);

    // POST-CONDITION 1: requestId IS now in journal — storeIntent was called.
    // Without storeIntent, isProcessed would still be false (no entry created).
    CHECK(journal.isProcessed("req-ota-1"),
          "POST: requestId IS in journal — storeIntent() was called (PENDING entry created)");

    // POST-CONDITION 2 (KEY BEHAVIORAL PROOF): state is EXACTLY EXECUTING.
    //
    // This is the strict assertion the auditor required:
    //   - State != PENDING  → markExecuting was called (PENDING → EXECUTING transition)
    //   - State != COMMITTED → commitTransaction was NOT called (download failed)
    //   - State == EXECUTING → markExecuting WAS called AND commitTransaction was NOT
    //
    // No OR-clause, no COMMITTED escape hatch. Deterministic behavioral proof.
    CHECK(journal.getTransactionState("req-ota-1") == TransactionState::EXECUTING,
          "POST: state == EXECUTING (strict) — PENDING → EXECUTING transitioned by markExecuting()");

    // POST-CONDITION 3 (defense-in-depth): state is NOT COMMITTED.
    // Confirms download did not succeed (commitTransaction was not called).
    CHECK(journal.getTransactionState("req-ota-1") != TransactionState::COMMITTED,
          "POST: state is NOT COMMITTED — commitTransaction() was not called (download failed)");

    // POST-CONDITION 4 (defense-in-depth): state is NOT PENDING.
    // Confirms markExecuting was actually called (otherwise state would be PENDING).
    // Note: getTransactionState returns PENDING for both EMPTY and PENDING records,
    // but POST-CONDITION 1 already confirmed the entry exists — so PENDING here
    // would mean "entry exists in PENDING state" (markExecuting not called).
    CHECK(journal.getTransactionState("req-ota-1") != TransactionState::PENDING,
          "POST: state is NOT PENDING — markExecuting() transitioned it to EXECUTING");

    // Reset OTA constant for subsequent tests
    Core::OTA_ED25519_PUBLIC_KEY_HEX = "";
  }

  // ---- TEST 10b: OTA via _handleOta() — Option A (pre-download validation) ----
  //
  // Defense-in-depth: a SECOND failure injection point at a DIFFERENT location
  // in the production validation chain. This proves the PENDING → EXECUTING
  // transition is robust regardless of WHERE the failure occurs after
  // markExecuting() was called.
  //
  // AUDITOR'S OPTION A (auditor called this "paling kuat" / strongest):
  //   Inject failure AFTER markExecuting() but BEFORE download begins — at
  //   the Ed25519 public key check (line 1695). Production returns at line
  //   1702, never reaching _downloadAndVerifyOta().
  printf("\n[TEST 10b] OTA via _handleOta() — PENDING → EXECUTING (Option A: pre-download validation)\n");
  {
    resetEnv();

    // ARM failure injection: OTA_ED25519_PUBLIC_KEY_HEX is empty (shim default).
    // Production _handleOta() fails at line 1695 — AFTER markExecuting was
    // called at line 1595, BEFORE _downloadAndVerifyOta() is reached.
    Core::OTA_ED25519_PUBLIC_KEY_HEX = "";  // explicit — failure injection armed
    CHECK(strlen(Core::OTA_ED25519_PUBLIC_KEY_HEX) == 0,
          "PRE: failure injection armed — OTA_ED25519_PUBLIC_KEY_HEX is empty");

    // PRE-CONDITION: requestId not in journal
    CHECK(!journal.isProcessed("req-ota-1"),
          "PRE: requestId 'req-ota-1' not in journal");

    // Same JSON as TEST 10 — failure happens earlier in validation chain
    // (signature length doesn't matter; Ed25519 key check fails first).
    const char* json = R"({"action":"update","requestId":"req-ota-1","url":"https://example.com/fw.bin","version":"4.1.0","size":100000,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","signature":"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"})";

    // Execute PRODUCTION _handleOta()
    mqtt._handleOta(json);

    // Same strict behavioral proof as TEST 10:
    //   - isProcessed == true  → storeIntent was called
    //   - state == EXECUTING  → markExecuting was called, PENDING → EXECUTING
    //   - state != COMMITTED   → commitTransaction was NOT called
    CHECK(journal.isProcessed("req-ota-1"),
          "POST: requestId IS in journal — storeIntent() was called");
    CHECK(journal.getTransactionState("req-ota-1") == TransactionState::EXECUTING,
          "POST: state == EXECUTING (strict) — markExecuting was called before validation failure");
    CHECK(journal.getTransactionState("req-ota-1") != TransactionState::COMMITTED,
          "POST: state is NOT COMMITTED — validation failed, no commitTransaction");
    CHECK(journal.getTransactionState("req-ota-1") != TransactionState::PENDING,
          "POST: state is NOT PENDING — markExecuting transitioned it to EXECUTING");
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
