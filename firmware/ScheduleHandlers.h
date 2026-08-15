// =============================================================================
// Web/Handlers/ScheduleHandlers.h — /api/schedule POST/DELETE
// =============================================================================
// P2-2 F-P0-2 C3: Refactored to use Web::Rest journal lifecycle helpers.
//
// COMMIT MODE: FROM_PENDING (atomic config mutation — no physical execution
// phase, no externally observable intermediate state).
//
// CRITICAL FIX (Phase B REV.3 §7.3): synchronous saveSchedule BEFORE commit.
// Previous version used markDirty() (deferred 10s save) which created a
// RAM/NVS divergence race: journal could say COMMITTED while schedule.json
// was not yet persisted. If device crashed in that window, schedule was lost.
//
// New flow (per Phase B REV.3 §7.3 + §9.5):
//   1. validate (auth, CSRF, body, fields)
//   2. [helper] validateRequestId + computeCommandHash + checkDuplicateAndRespond
//   3. [helper] storeIntentOrReject (PENDING)
//   4. [handler] mutate RAM (channels[idx].sched[])
//   5. [handler] saveScheduleWithResult(true) — SYNCHRONOUS NVS write
//      - On failure: HTTP 503, journal stays PENDING (INVARIANT B — RAM
//        mutation occurred, evidence preserved, NO clearEntry)
//   6. [handler] build ACK JSON
//   7. [helper] commitFromPendingOrFailure (COMMITTED)
//   8. [handler] send HTTP 200
//
// HARD INVARIANT: HTTP 200 only after commitFromPendingOrFailure returns true.
// HARD INVARIANT: saveSchedule failure → HTTP 503, journal PENDING, NO clearEntry.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_SCHEDULE_H
#define TIMER12_WEB_HANDLERS_SCHEDULE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RestJournalHelper.h"  // P2-2 F-P0-2 C3: journal lifecycle helpers
#include "ConfigStore.h"
#include "AuthManager.h"
#include "RelayEngine.h"
#include "Json.h"
#include "Config.h"
#include "Globals.h"
#include "RtcDriver.h"  // for ACK timestamp

namespace Web { namespace Handlers {

// POST /api/schedule { channelId, onTime, offTime, dayMask, enabled, id?, requestId }
// audit-fixes-v2 (auditor #4 P1-1): REST API previously accepted `action=toggle`
//   which is non-idempotent. MQTT command contract already removed toggle for
//   idempotency (only on/off/set_mode). REST API now matches — toggle is
//   rejected with 400. Idempotent mutations are critical for retry safety:
//   request → timeout → retry must not flip state twice.
inline void handleScheduleUpsert() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
  if (err) {
    sendError(400, "Invalid JSON");
    return;
  }

  // ---- Domain validation (channelId, time format, etc.) BEFORE journal ----
  int channelId = doc["channelId"] | 0;
  if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
    sendError(400, "Invalid channelId");
    return;
  }
  uint8_t idx = channelId - 1;
  const char* onTime = doc["onTime"] | "";
  const char* offTime = doc["offTime"] | "";
  if (strlen(onTime) != 5 || strlen(offTime) != 5) {
    sendError(400, "Invalid time format (use HH:MM)");
    return;
  }
  uint16_t onMin, offMin;
  if (!Utils::parseMinutes(onTime, onMin) || !Utils::parseMinutes(offTime, offMin)) {
    sendError(400, "Invalid time");
    return;
  }
  if (onMin == offMin) {
    sendError(400, "ON and OFF cannot be the same");
    return;
  }
  uint8_t dayMask = (uint8_t)(doc["dayMask"] | 0) & 0x7F;
  bool enabled = doc["enabled"] | true;
  int schedId = doc["id"] | 0;

  // For "Add new", check schedule limit BEFORE journal entry
  if (!(schedId > 0 && schedId <= Core::channels[idx].schedCount)) {
    if (Core::channels[idx].schedCount >= Core::MAX_SCHEDULES) {
      sendError(400, "Schedule limit reached (max 4 per channel)");
      return;
    }
  }

  // ---- [helper] requestId validation ----
  String requestId;
  if (!Web::Rest::validateRequestId(doc["requestId"] | "")) {
    return;
  }
  requestId = String(doc["requestId"] | "");

  // ---- [helper] command hash (uses shared Utils::computeCommandHash) ----
  // REST schedule body doesn't include "type"/"action" fields (those are MQTT
  // conventions). Inject them so the hash matches the MQTT canonical schema
  // for schedule commands (cross-ingress contract symmetry per §11).
  doc["type"] = "schedule";
  doc["action"] = "upsert";
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
  if (schedId > 0 && schedId <= Core::channels[idx].schedCount) {
    // Update existing
    uint8_t sIdx = schedId - 1;
    strncpy(Core::channels[idx].sched[sIdx].onTime, onTime, 5);
    Core::channels[idx].sched[sIdx].onTime[5] = '\0';
    strncpy(Core::channels[idx].sched[sIdx].offTime, offTime, 5);
    Core::channels[idx].sched[sIdx].offTime[5] = '\0';
    Core::channels[idx].sched[sIdx].onMin = onMin;
    Core::channels[idx].sched[sIdx].offMin = offMin;
    Core::channels[idx].sched[sIdx].dayMask = dayMask;
    Core::channels[idx].sched[sIdx].enabled = enabled;
  } else {
    // Add new (schedCount limit already checked above)
    uint8_t sIdx = Core::channels[idx].schedCount;
    strncpy(Core::channels[idx].sched[sIdx].onTime, onTime, 5);
    Core::channels[idx].sched[sIdx].onTime[5] = '\0';
    strncpy(Core::channels[idx].sched[sIdx].offTime, offTime, 5);
    Core::channels[idx].sched[sIdx].offTime[5] = '\0';
    Core::channels[idx].sched[sIdx].onMin = onMin;
    Core::channels[idx].sched[sIdx].offMin = offMin;
    Core::channels[idx].sched[sIdx].dayMask = dayMask;
    Core::channels[idx].sched[sIdx].enabled = enabled;
    Core::channels[idx].schedCount++;
  }
  Services::relayEngine.forceRefresh();

  // ---- [handler] SYNCHRONOUS saveSchedule (Phase B REV.3 §7.3) ----
  // This is the critical fix: persist schedule.json BEFORE committing the
  // journal. If this fails, journal stays PENDING (INVARIANT B).
  bool saved = Storage::config.saveScheduleWithResult(true);
  if (!saved) {
    Serial.printf("[REST] saveSchedule FAILED for rid=%s — preserving PENDING evidence\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "saveSchedule FAILED (REST) — RAM mutated, journal PENDING preserved: " + requestId, 0);
    // INVARIANT B: RAM mutation occurred, persistence failed.
    // DO NOT clearEntry — journal stays PENDING as evidence.
    // DO NOT commit — journal is NOT COMMITTED.
    Web::sendError(503,
      "DURABILITY_FAILURE: schedule persistence failed — RAM was updated but NVS write failed. "
      "Journal preserved as PENDING evidence. Please retry.");
    return;
  }

  // ---- [handler] build ACK JSON ----
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%llu",
           (unsigned long long)Drivers::rtc.getUnixTime() * 1000ULL);
  String ackJson = "{\"requestId\":\"";
  ackJson += requestId;
  ackJson += "\",\"success\":true,\"message\":\"Schedule saved\",\"timestamp\":";
  ackJson += tsBuf;
  char data[256];
  snprintf(data, sizeof(data),
           ",\"data\":{\"schedule\":{\"id\":%d,\"channelId\":%d,\"onTime\":\"%s\",\"offTime\":\"%s\",\"dayMask\":%d,\"enabled\":%s}}}",
           schedId > 0 ? schedId : (int)Core::channels[idx].schedCount,
           channelId, onTime, offTime, dayMask, enabled ? "true" : "false");
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

// DELETE /api/schedule?id=N&channelId=C  OR  DELETE /api/schedule?id=N
// audit-fixes-v2 (auditor #5 P1-2): schedule ID semantics were inconsistent:
//   - MQTT status published id = (channelId * 10) + scheduleIndex + 1 (composite)
//   - MQTT command upsert/delete used id = scheduleIndex within channel (1-4)
//   - REST POST used id = scheduleIndex within channel (matches MQTT command)
//   - REST DELETE used id = global sequential across all channels (1-48)
//   This meant the same schedule ID from /api/status could mean different
//   things in REST DELETE vs MQTT command.
//
//   Now REST DELETE accepts the composite ID format from MQTT status:
//     id = (channelId * 10) + scheduleIndex + 1
//   For backward compatibility, also accepts the old global-sequential format
//   when channelId query param is NOT provided AND id > 48 (impossible composite).
//   Preferred usage: pass channelId explicitly.
//     DELETE /api/schedule?channelId=3&id=2   (deletes channel 3 schedule index 2)
//     DELETE /api/schedule?id=32               (composite: channel 3 schedule 2)
//
// P2-2 F-P0-2 C3: DELETE now requires requestId (via query param ?requestId=...).
// The requestId is NOT in the JSON body for DELETE (no body). It's passed as
// a query parameter instead.
inline void handleScheduleDelete() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!Web::http.hasArg("id")) {
    sendError(400, "Missing id (use ?id=N&channelId=C or composite id=N)");
    return;
  }
  int id = Web::http.arg("id").toInt();
  if (id < 1) {
    sendError(400, "Invalid id");
    return;
  }

  uint8_t targetChannel = 0;
  uint8_t targetSchedIdx = 0;

  // audit-fixes-v2 (P1-2): prefer explicit channelId param.
  if (Web::http.hasArg("channelId")) {
    int chId = Web::http.arg("channelId").toInt();
    if (chId < 1 || chId > Core::NUM_CHANNELS) {
      sendError(400, "Invalid channelId (1-12)");
      return;
    }
    if (id < 1 || id > Core::MAX_SCHEDULES) {
      sendError(400, "Invalid schedule id (1-4 within channel)");
      return;
    }
    targetChannel = (uint8_t)(chId - 1);
    targetSchedIdx = (uint8_t)(id - 1);
  } else {
    // No channelId param — try composite ID format: id = (channelId * 10) + schedIdx + 1
    // Composite IDs are >= 11 (channel 1 schedule 1 = 11).
    if (id >= 11) {
      targetChannel = (uint8_t)((id / 10) - 1);
      targetSchedIdx = (uint8_t)((id % 10) - 1);
      if (targetChannel >= Core::NUM_CHANNELS || targetSchedIdx >= Core::MAX_SCHEDULES) {
        sendError(400, "Invalid composite id");
        return;
      }
    } else {
      // Legacy global-sequential format (id 1-48). Iterate channels.
      // audit-fixes-v2: kept for backward compat with old PWA builds.
      int remaining = id;
      for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
        if (remaining <= (int)Core::channels[i].schedCount) {
          targetChannel = i;
          targetSchedIdx = (uint8_t)(remaining - 1);
          break;
        }
        remaining -= Core::channels[i].schedCount;
      }
      if (targetChannel >= Core::NUM_CHANNELS) {
        sendError(404, "Schedule not found");
        return;
      }
    }
  }

  // Validate schedule exists at the computed slot
  if (targetSchedIdx >= Core::channels[targetChannel].schedCount) {
    sendError(404, "Schedule not found at specified slot");
    return;
  }

  // ---- [helper] requestId validation (via query param for DELETE) ----
  String requestId;
  if (Web::http.hasArg("requestId")) {
    requestId = Web::http.arg("requestId");
  } else {
    requestId = "";  // Will fail validation below
  }
  if (!Web::Rest::validateRequestId(requestId)) {
    return;
  }

  // ---- [helper] command hash for DELETE ----
  // For DELETE, the canonical hash is based on the delete target (channelId + id).
  // We build a minimal JSON doc for computeCommandHash.
  DynamicJsonDocument hashDoc(256);
  hashDoc["type"] = "schedule";
  hashDoc["action"] = "delete";
  hashDoc["channelId"] = (int)(targetChannel + 1);
  hashDoc["id"] = (int)(targetSchedIdx + 1);
  String commandHash = Web::Rest::computeCommandHash(hashDoc);

  // ---- [helper] duplicate check + ACK replay ----
  if (Web::Rest::checkDuplicateAndRespond(requestId, commandHash)) {
    return;
  }

  // ---- [helper] storeIntent (PENDING) ----
  if (!Web::Rest::storeIntentOrReject(requestId, commandHash,
                                        (uint8_t)(targetChannel + 1), false, false)) {
    return;
  }

  // ---- [handler] ACTUAL MUTATION (RAM) ----
  // Shift down
  for (uint8_t j = targetSchedIdx; j < Core::channels[targetChannel].schedCount - 1; j++) {
    Core::channels[targetChannel].sched[j] = Core::channels[targetChannel].sched[j + 1];
  }
  Core::channels[targetChannel].schedCount--;
  Services::relayEngine.forceRefresh();

  // ---- [handler] SYNCHRONOUS saveSchedule ----
  bool saved = Storage::config.saveScheduleWithResult(true);
  if (!saved) {
    Serial.printf("[REST] saveSchedule FAILED (delete) for rid=%s — preserving PENDING evidence\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "saveSchedule FAILED (REST delete) — RAM mutated, journal PENDING preserved: " + requestId, 0);
    Web::sendError(503,
      "DURABILITY_FAILURE: schedule persistence failed — RAM was updated but NVS write failed. "
      "Journal preserved as PENDING evidence. Please retry.");
    return;
  }

  int chId = targetChannel + 1;
  int scheduleId = targetSchedIdx + 1;
  Services::Log.append(Core::LogType::ConfigChange,
    "Schedule deleted via REST: CH" + String(chId) + " idx=" + String(scheduleId),
    chId);

  // ---- [handler] build ACK JSON ----
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%llu",
           (unsigned long long)Drivers::rtc.getUnixTime() * 1000ULL);
  String ackJson = "{\"requestId\":\"";
  ackJson += requestId;
  ackJson += "\",\"success\":true,\"message\":\"Schedule deleted\",\"timestamp\":";
  ackJson += tsBuf;
  char data[128];
  snprintf(data, sizeof(data),
           ",\"data\":{\"deleted\":true,\"channelId\":%d,\"scheduleId\":%d}}",
           chId, scheduleId);
  ackJson += data;
  ackJson += "}";

  // ---- [helper] commit (FROM_PENDING path) ----
  if (!Web::Rest::commitFromPendingOrFailure(requestId, ackJson)) {
    return;
  }

  // ---- [handler] send HTTP 200 ----
  Web::sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

}} // namespace Web::Handlers

#endif
