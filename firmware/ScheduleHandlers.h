// =============================================================================
// Web/Handlers/ScheduleHandlers.h — /api/schedule POST/DELETE
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

// POST /api/schedule { channelId, onTime, offTime, dayMask, enabled, id? }
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
  }
  Storage::config.markDirty();
  Services::relayEngine.forceRefresh();

  char data[256];
  snprintf(data, sizeof(data),
           "{\"schedule\":{\"id\":%d,\"channelId\":%d,\"onTime\":\"%s\",\"offTime\":\"%s\",\"dayMask\":%d,\"enabled\":%s}}",
           schedId > 0 ? schedId : (int)Core::channels[idx].schedCount,
           channelId, onTime, offTime, dayMask, enabled ? "true" : "false");
  sendSuccess("Schedule saved", data);
}

// DELETE /api/schedule?id=N
inline void handleScheduleDelete() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!Web::http.hasArg("id")) {
    sendError(400, "Missing id");
    return;
  }
  int id = Web::http.arg("id").toInt();
  if (id < 1) {
    sendError(400, "Invalid id");
    return;
  }
  // Search across all channels
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    if (id <= (int)Core::channels[i].schedCount) {
      // Shift down
      for (uint8_t j = id - 1; j < Core::channels[i].schedCount - 1; j++) {
        Core::channels[i].sched[j] = Core::channels[i].sched[j + 1];
      }
      Core::channels[i].schedCount--;
      Storage::config.markDirty();
      Services::relayEngine.forceRefresh();
      sendSuccess("Schedule deleted", "{\"deleted\":true}");
      return;
    }
    id -= Core::channels[i].schedCount;
  }
  sendError(404, "Schedule not found");
}

}} // namespace Web::Handlers

#endif
