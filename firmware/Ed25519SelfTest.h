// =============================================================================
// Ed25519SelfTest.h — On-target Ed25519 KAT self-test (RFC 8032 §7.1)
// -----------------------------------------------------------------------------
// Runs Ed25519 Known Answer Tests on the ACTUAL ESP32 hardware using the
// orlp/ed25519 library. Verifies that:
//   1. Valid RFC 8032 published test vectors are accepted
//   2. Tampered signatures are rejected
//   3. Wrong messages are rejected
//
// This closes the gap between "Python KAT passes" (tests reference impl) and
// "on-target KAT passes" (tests the actual ESP32 binary).
//
// Design decisions (per auditor brief):
//   - RFC 8032 PUBLISHED vectors (not self-consistency) — proves cross-impl compat
//   - Hardcoded test vectors — cannot be tampered by attacker
//   - 5-minute result cache — KAT ~400ms blocking; prevents repeated blocking
//   - esp_task_wdt_reset() between tests — defensive against watchdog
//   - Does NOT block OTA — KAT is diagnostic, not gate
//   - Does NOT modify orlp source — library stays audited
// =============================================================================
#pragma once
#ifndef TIMER12_ED25519_SELF_TEST_H
#define TIMER12_ED25519_SELF_TEST_H

#include <Arduino.h>
#include <cstdint>

namespace Services {

struct Ed25519KatResult {
  bool allPassed;
  int testsRun;
  int testsPassed;
  int testsFailed;
  unsigned long totalDurationMs;
  const char* libraryVersion;  // "orlp/ed25519"
  bool cached;                 // true if result came from cache
  unsigned long cachedAtMs;    // when cache was populated (millis())
};

class Ed25519SelfTest {
public:
  // Run the KAT (or return cached result if within 5 min).
  // This is BLOCKING (~400ms). Call from setup() or REST handler.
  Ed25519KatResult run(bool force = false);

  // Check if last run passed (non-blocking, uses cache).
  bool lastRunPassed() const { return _lastResult.allPassed; }

  // Get last result (non-blocking, uses cache).
  Ed25519KatResult getLastResult() const { return _lastResult; }

private:
  Ed25519KatResult _lastResult = {};
  bool _hasRun = false;

  // Individual test cases (each returns true on pass, false on fail)
  bool test_rfc8032_empty_message();
  bool test_rfc8032_1byte_message();
  bool test_negative_tampered_signature();
  bool test_negative_wrong_message();
};

extern Ed25519SelfTest ed25519SelfTest;

} // namespace Services

#endif
