// =============================================================================
// Web/Handlers/RelayHandlers.h — /api/relay
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_RELAY_H
#define TIMER12_WEB_HANDLERS_RELAY_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RelayEngine.h"
#include "AuthManager.h"
#include "Config.h"
#include "Globals.h"

namespace Web { namespace Handlers {

// POST /api/relay { channelId, action, mode?, manualState? }
// audit-fixes-v2 (auditor #4 P1-1): REST API previously accepted `action=toggle`
//   which is non-idempotent. MQTT command contract already removed toggle for
//   idempotency (only on/off/set_mode). REST API now matches — toggle is
//   rejected with 400. Idempotent mutations are critical for retry safety:
//   request → timeout → retry must not flip state twice.
inline void handleRelay() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
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
  const char* actionStr = doc["action"] | "";
  if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
    sendError(400, "Invalid channelId (1-12)");
    return;
  }
  uint8_t idx = channelId - 1;

  if (strcmp(actionStr, "on") == 0) {
    Services::relayEngine.setManual(idx, true);
  } else if (strcmp(actionStr, "off") == 0) {
    Services::relayEngine.setManual(idx, false);
  } else if (strcmp(actionStr, "set_mode") == 0) {
    const char* modeStr = doc["mode"] | "";
    bool manualState = doc["manualState"] | false;
    if (strcmp(modeStr, "auto") == 0) {
      Services::relayEngine.setMode(idx, true);
    } else if (strcmp(modeStr, "manual") == 0) {
      Services::relayEngine.setMode(idx, false);
      if (manualState) Services::relayEngine.setManual(idx, true);
      else Services::relayEngine.setManual(idx, false);
    } else {
      sendError(400, "Invalid mode");
      return;
    }
  } else {
    sendError(400, "Invalid action (use on/off/set_mode — toggle removed for idempotency)");
    return;
  }

  String data = "{\"channel\":{\"id\":";
  data += String(channelId);
  data += ",\"name\":\"";
  data += Core::channels[idx].name;
  data += "\",\"modeAuto\":";
  data += Core::channels[idx].modeAuto ? "true" : "false";
  data += ",\"manualState\":";
  data += Core::channels[idx].manualState ? "true" : "false";
  data += ",\"state\":";
  data += Core::relayState[idx] ? "true" : "false";
  const char* srcStr =
    Core::relaySource[idx] == Core::RelaySource::Manual ? "manual" :
    Core::relaySource[idx] == Core::RelaySource::Schedule ? "schedule" :
    Core::relaySource[idx] == Core::RelaySource::Pir ? "pir" : "off";
  data += ",\"source\":\"";
  data += srcStr;
  data += "\"}}";
  sendSuccess("Relay updated", data);
}

}} // namespace Web::Handlers

#endif
