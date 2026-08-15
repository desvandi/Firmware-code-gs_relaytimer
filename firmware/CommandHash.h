// =============================================================================
// Utils/CommandHash.h — Canonical command fingerprint (P2-2 F-P0-2 C1)
// =============================================================================
// EXTRACTED VERBATIM from MqttClient.cpp::_computeCommandHash (lines 2126-2175).
//
// AUDITOR CONSTRAINT (F-P0-2 Phase B REV.3 — FINAL APPROVAL):
//   "Jangan mengubah canonical hash schema selama refactor."
//   "Tidak boleh melakukan 'cleanup' canonicalization sekaligus extraction."
//   "Kalau ada perubahan hash algorithm, itu harus menjadi pekerjaan terpisah."
//
// This extraction is a PURE MOVE — no semantic changes. The canonical schema
// strings, field ordering, default values, and hash algorithm are IDENTICAL
// to the original. The only changes are:
//   1. `static` → `inline` (so the function can live in a header)
//   2. Function name `_computeCommandHash` → `computeCommandHash`
//      (drop private-convention leading underscore for shared API)
//   3. Moved from `namespace Services` (file-local) to `namespace Utils`
//      (cross-namespace utility — used by both Services::MqttClient and
//      Web::Rest helpers)
//
// USAGE:
//   Both MQTT path (MqttClient.cpp) and REST path (future RestJournalHelper.h)
//   call `Utils::computeCommandHash(doc)` to produce byte-identical hashes
//   for equivalent commands. This is the F-P0-2 §11 Cross-Ingress Contract.
//
// R10A-3 / R10C-1 (audit round 10C) — original comment preserved verbatim:
//
// Compute a deterministic command fingerprint.
//
// ENGINEER AUDIT FIX: Previous generic JSON-key iterator only handled string/int/bool.
// Other JSON types (float, array, object) were silently DROPPED from the hash,
// meaning requestId+commandHash binding was incomplete — attacker could add
// extra fields that don't affect the hash but DO affect execution.
//
// FIX: Per-command-type canonical schema. Each command type has a FIXED set
// of fields that are hashed in a DETERMINISTIC ORDER. Unknown fields cause
// the command to be REJECTED (not silently ignored).
//
// Canonical format: "type|action|field1=val1|field2=val2|..."
// =============================================================================
#pragma once
#ifndef TIMER12_UTILS_COMMAND_HASH_H
#define TIMER12_UTILS_COMMAND_HASH_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Crypto.h"  // for Utils::sha256Hex

namespace Utils {

// Compute canonical command fingerprint (SHA-256 hex string, 64 chars + null).
//
// Per-command-type schema (DO NOT MODIFY without separate audit pass —
// changing this invalidates all existing journal entries' commandHash fields):
//
//   relay    : "relay|<action>|channelId=N|mode=S|manualState=B"
//   schedule : "schedule|<action>|channelId=N|id=N|onTime=S|offTime=S|dayMask=N|enabled=B"
//   pir      : "pir|<action>|id=N|enabled=B|holdTime=N"
//   channel  : "channel|<action>|channelId=N|name=S"
//   time     : "time|<action>|datetime=S"
//   system   : "system|<action>"  (action only — reboot, getStatus, etc.)
//   config   : "config|<action>|deviceName=S|timezone=S"
//   ota      : "<action>|url=S|version=S|size=N|sha256=S|signature=S"
//              (NOTE: OTA commands have empty "type" field — see _handleOta)
//   (other)  : "<type>|<action>"  (no per-type fields — hash binds only type+action)
//
inline String computeCommandHash(const DynamicJsonDocument& doc) {
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

} // namespace Utils

#endif // TIMER12_UTILS_COMMAND_HASH_H
