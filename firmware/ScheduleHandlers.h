// =============================================================================
// Web/Handlers/ScheduleHandlers.h — /api/schedule POST/DELETE
// =============================================================================
// PD-001 (Phase 6): REST ingress now uses the SHARED CommandCanonicalizer +
//   TransactionJournal path. Canonical mappings:
//     POST   /api/schedule       →  type="schedule", action="upsert"
//     DELETE /api/schedule?id=..  →  type="schedule", action="delete"
//   Cross-transport hash equivalence: same logical schedule mutation via REST
//   or MQTT produces the SAME commandHash (AC-001/AC-018).
//
//   For DELETE, the canonical payload is {channelId, id} where id is the
//   1-based schedule index within the channel (matches MQTT semantics).
//   Composite/legacy ID formats are resolved to (channelId, id-within-channel)
//   BEFORE hashing so the hash is transport-independent.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_SCHEDULE_H
#define TIMER12_WEB_HANDLERS_SCHEDULE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "ConfigStore.h"
#include "AuthManager.h"
#include "RelayEngine.h"
#include "Json.h"
#include "Config.h"
#include "Globals.h"

namespace Web { namespace Handlers {

// POST /api/schedule { channelId, onTime, offTime, dayMask, enabled, id?, requestId }
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

  // --- PD-001: Canonical command model integration ---
  // REST endpoint implies (type="schedule", action="upsert").
  doc["type"] = "schedule";
  doc["action"] = "upsert";

  RestTransaction tx = beginTransaction(doc);
  if (!tx.ok) {
    sendError(400, tx.errorMessage);
    return;
  }
  if (tx.decision == Services::TransactionDecision::CONFLICT) {
    rejectConflict(tx);
    return;
  }
  if (tx.decision == Services::TransactionDecision::DUPLICATE) {
    replayDuplicate(tx);
    return;
  }

  // --- NEW: Execute ---
  int savedId = 0;
  if (schedId > 0 && schedId <= (int)Core::channels[idx].schedCount) {
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
    savedId = schedId;
  } else {
    // Add new
    if (Core::channels[idx].schedCount >= Core::MAX_SCHEDULES) {
      sendError(400, "Schedule limit reached (max 4 per channel)");
      return;
    }
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
    savedId = sIdx + 1;
  }
  Storage::config.markDirty();
  Services::relayEngine.forceRefresh();

  // --- Build success ACK JSON ---
  char data[384];
  snprintf(data, sizeof(data),
           "{\"schedule\":{\"id\":%d,\"channelId\":%d,\"onTime\":\"%s\",\"offTime\":\"%s\",\"dayMask\":%d,\"enabled\":%s},"
           "\"requestId\":\"%s\",\"commandHash\":\"%s\"}",
           savedId, channelId, onTime, offTime, dayMask, enabled ? "true" : "false",
           tx.transactionId.c_str(), tx.commandHash.c_str());

  String ackJson = "{\"success\":true,\"message\":\"Schedule saved\",\"data\":";
  ackJson += data;
  ackJson += "}";

  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  sendSecurityHeaders();
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
// PD-001 (Phase 6): For canonical hash determinism, the resolved
//   (channelId, id-within-channel) is what gets hashed — NOT the raw query
//   string. This ensures a DELETE via REST with `?channelId=3&id=2` produces
//   the SAME commandHash as an MQTT `type=schedule action=delete channelId=3 id=2`.
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

  // --- PD-001: Build canonical command doc ---
  // The canonical payload is {channelId, id} where id is 1-based index within
  // channel (matches MQTT semantics). requestId comes from query string.
  DynamicJsonDocument doc(256);
  doc["type"] = "schedule";
  doc["action"] = "delete";
  doc["channelId"] = (int)(targetChannel + 1);
  doc["id"] = (int)(targetSchedIdx + 1);
  String rid = Web::http.arg("requestId");
  if (rid.length() > 0) {
    doc["requestId"] = rid;
  }

  RestTransaction tx = beginTransaction(doc);
  if (!tx.ok) {
    sendError(400, tx.errorMessage);
    return;
  }
  if (tx.decision == Services::TransactionDecision::CONFLICT) {
    rejectConflict(tx);
    return;
  }
  if (tx.decision == Services::TransactionDecision::DUPLICATE) {
    replayDuplicate(tx);
    return;
  }

  // --- NEW: Execute ---
  // Shift down
  for (uint8_t j = targetSchedIdx; j < Core::channels[targetChannel].schedCount - 1; j++) {
    Core::channels[targetChannel].sched[j] = Core::channels[targetChannel].sched[j + 1];
  }
  Core::channels[targetChannel].schedCount--;
  Storage::config.markDirty();
  Services::relayEngine.forceRefresh();

  int channelId = targetChannel + 1;
  int scheduleId = targetSchedIdx + 1;
  Services::Log.append(Core::LogType::ConfigChange,
    "Schedule deleted via REST: CH" + String(channelId) + " idx=" + String(scheduleId),
    channelId);

  // --- Build success ACK JSON ---
  char data[256];
  snprintf(data, sizeof(data),
           "{\"deleted\":true,\"channelId\":%d,\"scheduleId\":%d,"
           "\"requestId\":\"%s\",\"commandHash\":\"%s\"}",
           channelId, scheduleId,
           tx.transactionId.c_str(), tx.commandHash.c_str());

  String ackJson = "{\"success\":true,\"message\":\"Schedule deleted\",\"data\":";
  ackJson += data;
  ackJson += "}";

  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

}} // namespace Web::Handlers

#endif
