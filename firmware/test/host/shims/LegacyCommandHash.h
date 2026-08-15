// =============================================================================
// LegacyCommandHash.h — TEST-ONLY pre-extraction oracle for F-P0-2 C1 audit
// =============================================================================
// PURPOSE: Provides an independent oracle to prove that the extraction of
// _computeCommandHash() from MqttClient.cpp (file-local static) to
// Utils::computeCommandHash() (shared inline in CommandHash.h) did NOT
// change edge-case semantics.
//
// This file contains a VERBATIM COPY of the original _computeCommandHash
// function body from MqttClient.cpp as it existed BEFORE the C1 extraction
// (commit f857973^, lines 2126-2175). The body is byte-identical to the
// original — no semantic modifications, no cleanup, no canonicalization
// changes. The only changes are:
//   1. `static` keyword removed (test file is single-TU, static is implicit)
//   2. Function renamed to `legacyComputeCommandHashForTest` (clearer intent)
//   3. Placed in global namespace (no namespace Services — Utils::sha256Hex
//      is available via the test shim)
//
// USAGE:
//   For each edge-case test (F1-F5), call BOTH:
//     String legacy = legacyComputeCommandHashForTest(doc);
//     String shared = Utils::computeCommandHash(doc);
//     assert(legacy == shared);
//
// This proves: the post-extraction shared function produces byte-identical
// output to the pre-extraction static function, even on edge inputs.
//
// The 14 baseline vectors (TEST 1-14) still use the original
// CommandHashBaseline.cpp capture approach (capture BEFORE extraction via
// production _handleCommand → journal.getCommandHash). That capture was
// performed against the original static impl and is therefore independent
// of the shared function.
//
// The 5 edge cases (F1-F5) are NEW additions in the C1 correction pass.
// They were NOT captured before extraction (because they didn't exist as
// test cases at that time). To avoid circular evidence (capturing the
// baseline WITH the shared function, then comparing against the same
// function), we use this legacy oracle.
//
// =============================================================================
#pragma once
#ifndef TIMER12_TEST_LEGACY_COMMAND_HASH_H
#define TIMER12_TEST_LEGACY_COMMAND_HASH_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Forward-declare Utils::sha256Hex — the test harness (MqttClientDeps.h)
// provides this via its own Utils:: namespace block. We do NOT include
// Crypto.h here because MqttClientDeps.h already guards it out
// (TIMER12_UTILS_CRYPTO_H) and provides its own Utils::sha256Hex impl.
// Including Crypto.h here would cause a redefinition conflict.
namespace Utils { String sha256Hex(const String& data); }

// VERBATIM COPY of MqttClient.cpp::_computeCommandHash (pre-C1-extraction).
// Source: git show f857973^:firmware/MqttClient.cpp lines 2126-2175
// Do NOT modify this body — any change defeats its purpose as an oracle.
inline String legacyComputeCommandHashForTest(const DynamicJsonDocument& doc) {
  const char* type = doc["type"] | "";
  const char* action = doc["action"] | "";

  String canonical = String(type) + "|" + String(action);

  // Per-command-type: extract ONLY the fields that affect execution.
  if (strcmp(type, "relay") == 0) {
    canonical += "|channelId=" + String(doc["channelId"] | 0);
    canonical += "|mode=" + String(doc["mode"] | "");
    canonical += "|manualState=" + String(doc["manualState"] | false ? "true" : "false");
  }
  else if (strcmp(type, "schedule") == 0) {
    canonical += "|channelId=" + String(doc["channelId"] | 0);
    canonical += "|id=" + String(doc["id"] | 0);
    canonical += "|onTime=" + String(doc["onTime"] | "");
    canonical += "|offTime=" + String(doc["offTime"] | "");
    canonical += "|dayMask=" + String(doc["dayMask"] | 0);
    canonical += "|enabled=" + String(doc["enabled"] | true ? "true" : "false");
  }
  else if (strcmp(type, "pir") == 0) {
    canonical += "|id=" + String(doc["id"] | 0);
    canonical += "|enabled=" + String(doc["enabled"] | false ? "true" : "false");
    canonical += "|holdTime=" + String(doc["holdTime"] | 0);
  }
  else if (strcmp(type, "channel") == 0) {
    canonical += "|channelId=" + String(doc["channelId"] | 0);
    canonical += "|name=" + String(doc["name"] | "");
  }
  else if (strcmp(type, "time") == 0) {
    canonical += "|datetime=" + String(doc["datetime"] | "");
  }
  else if (strcmp(type, "system") == 0) {
    // system commands: action only (reboot, getStatus, resetEnergyStats, resetDailyStats)
  }
  else if (strcmp(type, "config") == 0) {
    canonical += "|deviceName=" + String(doc["deviceName"] | "");
    canonical += "|timezone=" + String(doc["timezone"] | "");
  }
  else if (strcmp(type, "ota") == 0) {
    // R10C-1 FIX: OTA command hash — was UNDEFINED in _handleOta() (compile error).
    canonical += "|url=" + String(doc["url"] | "");
    canonical += "|version=" + String(doc["version"] | "");
    canonical += "|size=" + String((unsigned long)(doc["size"] | 0));
    canonical += "|sha256=" + String(doc["sha256"] | "");
    canonical += "|signature=" + String(doc["signature"] | "");
  }

  return Utils::sha256Hex(canonical);
}

#endif // TIMER12_TEST_LEGACY_COMMAND_HASH_H
