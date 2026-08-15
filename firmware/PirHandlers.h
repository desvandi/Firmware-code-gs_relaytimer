// =============================================================================
// Web/Handlers/PirHandlers.h — /api/pir (config), /api/pir/test
// =============================================================================
// P2-2 F-P0-2 C5: handlePirConfig refactored to use Web::Rest journal
// lifecycle helpers. handlePirTest is OUT OF C5 SCOPE (physical trigger,
// not a config mutation — would need EXECUTING mode like relay, different
// pattern — deferred to future phase).
//
// COMMIT MODE: FROM_PENDING (atomic config mutation — no physical execution
// phase, no externally observable intermediate state).
//
// CRITICAL FIX (Phase B REV.3 §7.3, same as C3 schedule + C4 channel):
// synchronous persistence BEFORE commit. Previous version used markDirty()
// (deferred 10s save) which created a RAM/NVS divergence race: journal could
// say COMMITTED while schedule.json (which also stores PIR config) was not
// yet persisted. If device crashed in that window, PIR config was lost.
//
// New flow (per Phase B REV.3 §7.3 + §9.5, same pattern as C3/C4):
//   1. validate (auth, CSRF, body, fields)
//   2. [helper] validateRequestId + computeCommandHash + checkDuplicateAndRespond
//   3. [helper] storeIntentOrReject (PENDING)
//   4. [handler] mutate RAM (channels[chIdx].pirEnabled / pirHoldTime)
//   5. [handler] saveScheduleWithResult(true) — SYNCHRONOUS NVS write
//      - PIR config (pirEnabled, pirHoldTime) is stored in schedule.json
//        alongside channel names and schedules (verified in ConfigStore.cpp:268-269
//        — ch["pirEnabled"] + ch["pirHoldTime"] are written in the same loop).
//      - On failure: HTTP 503, journal stays PENDING (INVARIANT B — RAM
//        mutation occurred, evidence preserved, NO clearEntry)
//   6. [handler] build ACK JSON
//   7. [helper] commitFromPendingOrFailure (COMMITTED)
//   8. [handler] send HTTP 200
//
// HARD INVARIANT: HTTP 200 only after commitFromPendingOrFailure returns true.
// HARD INVARIANT: saveSchedule failure → HTTP 503, journal PENDING, NO clearEntry.
//
// CROSS-INGRESS CONTRACT (Phase B REV.3 §11): REST handler injects
// doc["type"]="pir" and doc["action"]="config" before computeCommandHash
// so the hash matches the MQTT canonical schema for PIR config commands.
// This is NOT a REST-specific hash — it uses the same Utils::computeCommandHash()
// single source of truth.
//
// PIR field mapping:
//   PIR id 1-4 maps to channels[PIR_CHANNEL_OFFSET + id - 1] = channels[8..11]
//   pirEnabled and pirHoldTime are fields on the Channel struct (shared with
//   relay channel config). This is why saveScheduleWithResult() persists them.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_PIR_H
#define TIMER12_WEB_HANDLERS_PIR_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RestJournalHelper.h"  // P2-2 F-P0-2 C5: journal lifecycle helpers
#include "PirDriver.h"
#include "AuthManager.h"
#include "ConfigStore.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include "RtcDriver.h"  // for ACK timestamp

namespace Web { namespace Handlers {

// POST /api/pir { id, enabled?, holdTime?, requestId }
//   Updates PIR config (enabled, holdTime) for PIR id 1-4.
//   PIR config is persisted to schedule.json via saveScheduleWithResult.
inline void handlePirConfig() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(256)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
  if (err) {
    sendError(400, "Invalid JSON");
    return;
  }

  // ---- Domain validation (id range) BEFORE journal ----
  int id = doc["id"] | 0;
  if (id < 1 || id > (int)Core::NUM_PIR) {
    sendError(400, "Invalid PIR id (1-4)");
    return;
  }
  uint8_t idx = id - 1;
  uint8_t chIdx = Core::PIR_CHANNEL_OFFSET + idx;

  // ---- [helper] requestId validation ----
  String requestId;
  if (!Web::Rest::validateRequestId(doc["requestId"] | "")) {
    return;
  }
  requestId = String(doc["requestId"] | "");

  // ---- [helper] command hash (uses shared Utils::computeCommandHash) ----
  // REST PIR body doesn't include "type"/"action" fields (those are MQTT
  // conventions). Inject them so the hash matches the MQTT canonical schema
  // for PIR config commands (cross-ingress contract symmetry per §11).
  doc["type"] = "pir";
  doc["action"] = "config";
  String commandHash = Web::Rest::computeCommandHash(doc);

  // ---- [helper] duplicate check + ACK replay ----
  if (Web::Rest::checkDuplicateAndRespond(requestId, commandHash)) {
    return;
  }

  // ---- [helper] storeIntent (PENDING) ----
  if (!Web::Rest::storeIntentOrReject(requestId, commandHash,
                                        (uint8_t)id, false, false)) {
    return;
  }

  // ---- [handler] ACTUAL MUTATION (RAM) ----
  // After this point, INVARIANT B applies: if anything fails, journal MUST
  // stay PENDING/EXECUTING — NO clearEntry (RAM already mutated).
  if (doc.containsKey("enabled")) {
    Core::channels[chIdx].pirEnabled = doc["enabled"].as<bool>();
  }
  if (doc.containsKey("holdTime")) {
    int ht = doc["holdTime"] | 120;
    if (ht < 5) ht = 5;
    if (ht > 600) ht = 600;
    Core::channels[chIdx].pirHoldTime = (uint16_t)ht;
  }

  // ---- [handler] SYNCHRONOUS saveSchedule (Phase B REV.3 §7.3) ----
  // PIR config (pirEnabled, pirHoldTime) is stored in schedule.json alongside
  // channel names and schedules (verified in ConfigStore.cpp:268-269). So
  // saveScheduleWithResult() persists all three (channels + schedules + PIR).
  // This is the critical fix: persist BEFORE committing the journal. If this
  // fails, journal stays PENDING (INVARIANT B).
  bool saved = Storage::config.saveScheduleWithResult(true);
  if (!saved) {
    Serial.printf("[REST] saveSchedule FAILED for PIR config rid=%s — preserving PENDING evidence\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "saveSchedule FAILED (REST PIR config) — RAM mutated, journal PENDING preserved: " + requestId, 0);
    // INVARIANT B: RAM mutation occurred, persistence failed.
    // DO NOT clearEntry — journal stays PENDING as evidence.
    // DO NOT commit — journal is NOT COMMITTED.
    Web::sendError(503,
      "DURABILITY_FAILURE: PIR config persistence failed — RAM was updated but NVS write failed. "
      "Journal preserved as PENDING evidence. Please retry.");
    return;
  }

  Services::Log.append(Core::LogType::ConfigChange,
    "PIR " + String(id) + " config via REST", chIdx + 1);

  // ---- [handler] build ACK JSON ----
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%llu",
           (unsigned long long)Drivers::rtc.getUnixTime() * 1000ULL);
  String ackJson = "{\"requestId\":\"";
  ackJson += requestId;
  ackJson += "\",\"success\":true,\"message\":\"PIR config updated\",\"timestamp\":";
  ackJson += tsBuf;
  char data[192];
  snprintf(data, sizeof(data),
           ",\"data\":{\"pir\":{\"id\":%d,\"channelId\":%d,\"enabled\":%s,\"holdTime\":%u}}}",
           id, chIdx + 1,
           Core::channels[chIdx].pirEnabled ? "true" : "false",
           Core::channels[chIdx].pirHoldTime);
  ackJson += data;
  ackJson += "}";

  // ---- [helper] commit (FROM_PENDING path) ----
  if (!Web::Rest::commitFromPendingOrFailure(requestId, ackJson)) {
    // Helper already sent HTTP 503 — DO NOT send HTTP 200
    // Journal stays PENDING (INVARIANT B — RAM mutated, save succeeded but
    // journal commit failed). clearEntry is FORBIDDEN.
    return;
  }

  // ---- [handler] send HTTP 200 ----
  Web::sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

// POST /api/pir/test { id }
//   Triggers a PIR test motion event (physical action, not config mutation).
//   OUT OF C5 SCOPE — not refactored to use journal (would need EXECUTING
//   mode like relay, different pattern — deferred to future phase).
inline void handlePirTest() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(64)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  DynamicJsonDocument doc(64);
  DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
  if (err) {
    sendError(400, "Invalid JSON");
    return;
  }
  int id = doc["id"] | 0;
  if (id < 1 || id > (int)Core::NUM_PIR) {
    sendError(400, "Invalid PIR id (1-4)");
    return;
  }
  if (millis() < Core::pirStartupTime + Core::PIR_WARMUP_MS) {
    sendError(400, "PIR in warm-up");
    return;
  }
  Drivers::pir.testTrigger(id - 1);
  sendSuccess("PIR triggered", "{\"triggered\":true}");
}

}} // namespace Web::Handlers

#endif
