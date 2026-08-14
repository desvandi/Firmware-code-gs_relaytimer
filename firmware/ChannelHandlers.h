// =============================================================================
// Web/Handlers/ChannelHandlers.h — /api/channel (rename channel)
// =============================================================================
// audit-fixes-v2 (auditor #5 P1-1):
//   PWA calls POST /api/channel to rename a channel. The mock Next.js API
//   route exists (src/app/api/channel/route.ts), but firmware REST server
//   never registered /api/channel → PWA LAN REST mode got 404. MQTT mode
//   worked because MqttClient.cpp handles type="channel" action="rename".
//   This file adds the missing REST endpoint so both modes are consistent.
#pragma once
#ifndef TIMER12_WEB_HANDLERS_CHANNEL_H
#define TIMER12_WEB_HANDLERS_CHANNEL_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "AuthManager.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"

namespace Web { namespace Handlers {

// POST /api/channel { channelId, name }
//   Renames a relay channel (1-12). Name max 20 chars (MAX_NAME_LEN).
//   Persists to schedule.json via ConfigStore.
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

  uint8_t idx = channelId - 1;
  strncpy(Core::channels[idx].name, newName.c_str(), Core::MAX_NAME_LEN);
  Core::channels[idx].name[Core::MAX_NAME_LEN] = '\0';
  Storage::config.saveSchedule();

  Services::Log.append(Core::LogType::ConfigChange,
    "CH" + String(channelId) + " renamed via REST: " + newName, channelId);

  String data = "{\"channel\":{\"id\":";
  data += String(channelId);
  data += ",\"name\":\"";
  data += Core::channels[idx].name;
  data += "\"}}";
  sendSuccess("Channel renamed", data);
}

}} // namespace Web::Handlers

#endif
