// =============================================================================
// Web/Handlers/PirHandlers.h — /api/pir, /api/pir/test
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_PIR_H
#define TIMER12_WEB_HANDLERS_PIR_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "PirDriver.h"
#include "AuthManager.h"
#include "ConfigStore.h"
#include "Config.h"
#include "Globals.h"

namespace Web { namespace Handlers {

// POST /api/pir { id, enabled?, holdTime? }
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
  int id = doc["id"] | 0;
  if (id < 1 || id > (int)Core::NUM_PIR) {
    sendError(400, "Invalid PIR id (1-4)");
    return;
  }
  uint8_t idx = id - 1;
  uint8_t chIdx = Core::PIR_CHANNEL_OFFSET + idx;
  if (doc.containsKey("enabled")) {
    Core::channels[chIdx].pirEnabled = doc["enabled"].as<bool>();
  }
  if (doc.containsKey("holdTime")) {
    int ht = doc["holdTime"] | 120;
    if (ht < 5) ht = 5;
    if (ht > 600) ht = 600;
    Core::channels[chIdx].pirHoldTime = (uint16_t)ht;
  }
  Storage::config.markDirty();

  String data = "{\"pir\":{\"id\":";
  data += String(id);
  data += ",\"channelId\":";
  data += String(chIdx + 1);
  data += ",\"enabled\":";
  data += Core::channels[chIdx].pirEnabled ? "true" : "false";
  data += ",\"holdTime\":";
  data += String(Core::channels[chIdx].pirHoldTime);
  data += "}}";
  sendSuccess("PIR config updated", data);
}

// POST /api/pir/test { id }
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
