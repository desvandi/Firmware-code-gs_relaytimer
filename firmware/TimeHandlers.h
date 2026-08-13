// =============================================================================
// Web/Handlers/TimeHandlers.h — /api/time
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_TIME_H
#define TIMER12_WEB_HANDLERS_TIME_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RtcDriver.h"
#include "AuthManager.h"
#include "Json.h"
#include "Config.h"

namespace Web { namespace Handlers {

// POST /api/time { datetime: "YYYY-MM-DDTHH:MM:SS" }
inline void handleSetTime() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(128)) return;
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
  const char* dt = doc["datetime"] | "";
  int y, m, d, h, mi, s;
  if (sscanf(dt, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &mi, &s) != 6) {
    sendError(400, "Invalid datetime (use ISO 8601: YYYY-MM-DDTHH:MM:SS)");
    return;
  }
  if (!Utils::isValidDate(y, m, d) || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
    sendError(400, "Invalid date/time components");
    return;
  }
  Drivers::rtc.adjust(y, m, d, h, mi, s);
  sendSuccess("RTC time synced", "{\"synced\":true}");
}

}} // namespace Web::Handlers

#endif
