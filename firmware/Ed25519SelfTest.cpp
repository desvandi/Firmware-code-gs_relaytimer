// =============================================================================
// Ed25519SelfTest.cpp — On-target Ed25519 KAT self-test (RFC 8032 §7.1)
// =============================================================================
#include "Ed25519SelfTest.h"
#include "ed25519.h"        // orlp/ed25519 audited library
#include "LogService.h"
#include "Globals.h"
#include <esp_task_wdt.h>
#include <cstring>

namespace Services {

Ed25519SelfTest ed25519SelfTest;

// ============================================================================
// RFC 8032 §7.1 Test 1: Empty message
// ============================================================================
static const uint8_t T1_PUBKEY[32] = {
    0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xdf,0x3c,0x89,0x76,0x50,
    0x7d,0xe6,0xb6,0x1a,0xe5,0x8d,0x9d,0x35,0xe1,0x2a,0x3f,0x5d,0x5f,0xc6,0x01,0x71
};
static const uint8_t T1_SIG[64] = {
    0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,
    0x84,0x87,0x7f,0x1e,0xb8,0xe5,0xd9,0x74,0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
    0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,0xc6,0xe3,0x69,0x2c,0x62,0x1a,0xab,0x6f,
    0x5d,0x0c,0xe7,0x8e,0x83,0x29,0x18,0x4c,0x32,0xc0,0x95,0x6b,0x5f,0x72,0x4c,0x05
};
// T1 message: empty (0 bytes)

// ============================================================================
// RFC 8032 §7.1 Test 2: 1-byte message (0x72)
// ============================================================================
static const uint8_t T2_PUBKEY[32] = {
    0x3d,0x40,0x17,0xc3,0xe8,0x43,0x89,0x5a,0x92,0xb7,0x0a,0xa7,0xd1,0xdc,0xb7,0x01,
    0x80,0x5c,0xbf,0x01,0xa5,0x9f,0xb9,0xd8,0xb0,0x3c,0xd5,0xe3,0xf8,0xee,0xe7,0x53
};
static const uint8_t T2_SIG[64] = {
    0x92,0xa0,0x09,0xa9,0xf0,0xd4,0xca,0xb8,0x72,0x0e,0x82,0x0b,0x5f,0x64,0x25,0x40,
    0xf2,0x19,0xe0,0xf2,0x60,0x58,0x73,0x11,0xae,0xf8,0xf1,0x0c,0x9d,0x04,0x65,0x1d,
    0x68,0x53,0xb5,0x07,0xd9,0x71,0x86,0xa4,0xe2,0x2c,0x7e,0x9a,0x43,0x61,0x16,0x2e,
    0x39,0x54,0xe7,0x6a,0xc3,0x14,0xa6,0x0d,0x4b,0x40,0x15,0x09,0xd0,0x49,0x4c,0x04
};
static const uint8_t T2_MSG[1] = { 0x72 };

// ============================================================================
// Cache duration: 5 minutes (300000 ms)
// ============================================================================
static constexpr unsigned long KAT_CACHE_MS = 300000UL;

// ============================================================================
// Individual test cases
// ============================================================================

bool Ed25519SelfTest::test_rfc8032_empty_message() {
  // Test 1: RFC 8032 §7.1 — empty message, valid signature MUST verify
  int result = ed25519_verify(T1_SIG, NULL, 0, T1_PUBKEY);
  if (result == 1) {
    Serial.println("[Ed25519-KAT] Test 1 (RFC 8032 empty msg): PASS");
    return true;
  } else {
    Serial.println("[Ed25519-KAT] Test 1 (RFC 8032 empty msg): FAIL — valid signature rejected!");
    return false;
  }
}

bool Ed25519SelfTest::test_rfc8032_1byte_message() {
  // Test 2: RFC 8032 §7.1 — 1-byte message (0x72), valid signature MUST verify
  int result = ed25519_verify(T2_SIG, T2_MSG, 1, T2_PUBKEY);
  if (result == 1) {
    Serial.println("[Ed25519-KAT] Test 2 (RFC 8032 1-byte msg): PASS");
    return true;
  } else {
    Serial.println("[Ed25519-KAT] Test 2 (RFC 8032 1-byte msg): FAIL — valid signature rejected!");
    return false;
  }
}

bool Ed25519SelfTest::test_negative_tampered_signature() {
  // Test N1: Tampered signature (flip 1 bit) — MUST be rejected
  uint8_t bad_sig[64];
  memcpy(bad_sig, T1_SIG, 64);
  bad_sig[0] ^= 0x01;  // flip one bit
  int result = ed25519_verify(bad_sig, NULL, 0, T1_PUBKEY);
  if (result == 0) {
    Serial.println("[Ed25519-KAT] Test N1 (tampered sig): PASS (correctly rejected)");
    return true;
  } else {
    Serial.println("[Ed25519-KAT] Test N1 (tampered sig): FAIL — tampered signature accepted!");
    return false;
  }
}

bool Ed25519SelfTest::test_negative_wrong_message() {
  // Test N2: Valid signature + wrong message — MUST be rejected
  static const uint8_t WRONG_MSG[4] = { 'T', 'E', 'S', 'T' };
  int result = ed25519_verify(T2_SIG, WRONG_MSG, 4, T2_PUBKEY);
  if (result == 0) {
    Serial.println("[Ed25519-KAT] Test N2 (wrong msg): PASS (correctly rejected)");
    return true;
  } else {
    Serial.println("[Ed25519-KAT] Test N2 (wrong msg): FAIL — wrong message accepted!");
    return false;
  }
}

// ============================================================================
// Main runner — runs all 4 tests, returns aggregated result
// ============================================================================
Ed25519KatResult Ed25519SelfTest::run(bool force) {
  // Return cached result if within 5 minutes and not forced
  if (_hasRun && !force &&
      (millis() - _lastResult.cachedAtMs < KAT_CACHE_MS)) {
    Ed25519KatResult cached = _lastResult;
    cached.cached = true;
    return cached;
  }

  Serial.println("[Ed25519-KAT] Starting on-target Ed25519 KAT self-test...");
  unsigned long startTime = millis();

  Ed25519KatResult result = {};
  result.libraryVersion = "orlp/ed25519";
  result.cached = false;
  result.testsRun = 4;

  // Test 1: RFC 8032 empty message
  bool t1 = test_rfc8032_empty_message();
  esp_task_wdt_reset();  // defensive — reset watchdog between tests

  // Test 2: RFC 8032 1-byte message
  bool t2 = test_rfc8032_1byte_message();
  esp_task_wdt_reset();

  // Test N1: Negative — tampered signature
  bool t3 = test_negative_tampered_signature();
  esp_task_wdt_reset();

  // Test N2: Negative — wrong message
  bool t4 = test_negative_wrong_message();
  esp_task_wdt_reset();

  result.testsPassed = (t1 ? 1 : 0) + (t2 ? 1 : 0) + (t3 ? 1 : 0) + (t4 ? 1 : 0);
  result.testsFailed = result.testsRun - result.testsPassed;
  result.allPassed = (result.testsFailed == 0);
  result.totalDurationMs = millis() - startTime;
  result.cachedAtMs = millis();

  // Store in cache
  _lastResult = result;
  _hasRun = true;

  // Log result
  Serial.printf("[Ed25519-KAT] Complete: %d/%d passed in %lu ms\n",
                result.testsPassed, result.testsRun, result.totalDurationMs);
  if (result.allPassed) {
    Serial.println("[Ed25519-KAT] ✅ ALL TESTS PASSED — Ed25519 verification functional on this device");
    Services::Log.append(Core::LogType::Ota,
      "Ed25519 KAT PASSED (" + String(result.testsPassed) + "/" + String(result.testsRun) + ")", 0);
  } else {
    Serial.println("[Ed25519-KAT] ❌ SOME TESTS FAILED — Ed25519 may not work correctly on this device");
    Services::Log.append(Core::LogType::Error,
      "Ed25519 KAT FAILED (" + String(result.testsFailed) + " tests failed)", 0);
  }

  return result;
}

} // namespace Services
