// =============================================================================
// Web/Handlers/ChannelHandlers.h — /api/channel POST (rename)
// =============================================================================
// P2-2 F-P0-2 C4: Refactored to use Web::Rest journal lifecycle helpers.
//
// COMMIT MODE: FROM_PENDING (atomic config mutation — no physical execution
// phase, no externally observable intermediate state).
//
// CRITICAL FIX (Phase B REV.3 §7.3, same as C3 schedule): synchronous
// persistence BEFORE commit. Previous version used saveSchedule() (which
// delegates to saveScheduleWithResult but ignores the result, with the
// actual write deferred via markDirty()). This created a RAM/NVS divergence
// race: journal could say COMMITTED while schedule.json (which also stores
// channel names) was not yet persisted. If device crashed in that window,
// channel rename was lost.
//
// New flow (per Phase B REV.3 §7.3 + §9.5, same pattern as C3 schedule):
//   1. validate (auth, CSRF, body, fields)
//   2. [helper] validateRequestId + computeCommandHash + checkDuplicateAndRespond
//   3. [helper] storeIntentOrReject (PENDING)
//   4. [handler] mutate RAM (channels[idx].name)
//   5. [handler] saveScheduleWithResult(true) — SYNCHRONOUS NVS write
//      - Channel names are stored in schedule.json alongside schedules,
//        so saveScheduleWithResult() persists both. No separate save
//        method needed (verified in ConfigStore.cpp:265 — ch["name"] is
//        written in the same loop as schedules).
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
// doc["type"]="channel" and doc["action"]="rename" before computeCommandHash
// so the hash matches the MQTT canonical schema for channel rename commands.
// This is NOT a REST-specific hash — it uses the same Utils::computeCommandHash()
// single source of truth.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_CHANNEL_H
#define TIMER12_WEB_HANDLERS_CHANNEL_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RestJournalHelper.h"  // P2-2 F-P0-2 C4: journal lifecycle helpers
#include "ConfigStore.h"
#include "AuthManager.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include "RtcDriver.h"  // for ACK timestamp

namespace Web { namespace Handlers {

// POST /api/channel { channelId, name, requestId }
//   Renames a relay channel (1-12). Name max 20 chars (MAX_NAME_LEN).
//   Channel names are persisted to schedule.json (alongside schedules).
//
// audit-fixes-v2 (auditor #5 P1-1):
//   PWA calls POST /api/channel to rename a channel. The mock Next.js API
//   route exists (src/app/api/channel/route.ts), but firmware REST server
//   never registered /api/channel → PWA LAN REST mode got 404. MQTT mode
//   worked because MqttClient.cpp handles type="channel" action="rename".
//   This file adds the missing REST endpoint so both modes are consistent.
inline void handleChannelRename() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(256)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
  if (err) {
    sendError(400, "Invalid JSON");
    return;
  }

  // ---- Domain validation (channelId, name length) BEFORE journal ----
  int channelId = doc["channelId"] | 0;
  const char* name = doc["name"] | "";
  if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
    sendError(400, "Invalid channelId (1-12)");
    return;
  }
  String newName = String(name);
  newName.trim();
  if (newName.length() < 1 || newName.length() > Core::MAX_NAME_LEN) {
    sendError(400, "Name must be 1-20 chars");
    return;
  }

  // ---- [helper] requestId validation ----
  String requestId;
  if (!Web::Rest::validateRequestId(doc["requestId"] | "")) {
    return;
  }
  requestId = String(doc["requestId"] | "");

  // ---- [helper] command hash (uses shared Utils::computeCommandHash) ----
  // REST channel body doesn't include "type"/"action" fields (those are MQTT
  // conventions). Inject them so the hash matches the MQTT canonical schema
  // for channel rename commands (cross-ingress contract symmetry per §11).
  doc["type"] = "channel";
  doc["action"] = "rename";
  String commandHash = Web::Rest::computeCommandHash(doc);

  // ---- [helper] duplicate check + ACK replay ----
  if (Web::Rest::checkDuplicateAndRespond(requestId, commandHash)) {
    return;
  }

  // ---- [helper] storeIntent (PENDING) ----
  if (!Web::Rest::storeIntentOrReject(requestId, commandHash,
                                        (uint8_t)channelId, false, false)) {
    return;
  }

  // ---- [handler] ACTUAL MUTATION (RAM) ----
  // After this point, INVARIANT B applies: if anything fails, journal MUST
  // stay PENDING/EXECUTING — NO clearEntry (RAM already mutated).
  uint8_t idx = channelId - 1;
  strncpy(Core::channels[idx].name, newName.c_str(), Core::MAX_NAME_LEN);
  Core::channels[idx].name[Core::MAX_NAME_LEN] = '\0';

  // ---- [handler] SYNCHRONOUS saveSchedule (Phase B REV.3 §7.3) ----
  // Channel names are stored in schedule.json alongside schedules (verified
  // in ConfigStore.cpp:265 — ch["name"] is written in the same loop as
  // schedules). So saveScheduleWithResult() persists both channels and
  // schedules. This is the critical fix: persist BEFORE committing the
  // journal. If this fails, journal stays PENDING (INVARIANT B).
  bool saved = Storage::config.saveScheduleWithResult(true);
  if (!saved) {
    Serial.printf("[REST] saveSchedule FAILED for channel rename rid=%s — preserving PENDING evidence\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "saveSchedule FAILED (REST channel rename) — RAM mutated, journal PENDING preserved: " + requestId, 0);
    // INVARIANT B: RAM mutation occurred, persistence failed.
    // DO NOT clearEntry — journal stays PENDING as evidence.
    // DO NOT commit — journal is NOT COMMITTED.
    Web::sendError(503,
      "DURABILITY_FAILURE: channel rename persistence failed — RAM was updated but NVS write failed. "
      "Journal preserved as PENDING evidence. Please retry.");
    return;
  }

  Services::Log.append(Core::LogType::ConfigChange,
    "CH" + String(channelId) + " renamed via REST: " + newName, channelId);

  // ---- [handler] build ACK JSON ----
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%llu",
           (unsigned long long)Drivers::rtc.getUnixTime() * 1000ULL);
  String ackJson = "{\"requestId\":\"";
  ackJson += requestId;
  ackJson += "\",\"success\":true,\"message\":\"Channel renamed\",\"timestamp\":";
  ackJson += tsBuf;
  char data[160];
  snprintf(data, sizeof(data),
           ",\"data\":{\"channel\":{\"channelId\":%d,\"name\":\"%s\"}}}",
           channelId, Core::channels[idx].name);
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

}} // namespace Web::Handlers

#endif
