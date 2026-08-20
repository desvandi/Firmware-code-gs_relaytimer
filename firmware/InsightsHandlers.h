// =============================================================================
// Web/Handlers/InsightsHandlers.h — /api/insights (Phase B / P0-01)
// -----------------------------------------------------------------------------
// Phase B: PWA fetches insights from ESP32's authenticated REST endpoint.
// ESP32 holds the device HMAC secret in NVS and proxies the request to GAS.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_INSIGHTS_H
#define TIMER12_WEB_HANDLERS_INSIGHTS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "Config.h"
#include "Globals.h"
#include "Advisor.h"
#include "LogService.h"

namespace Web { namespace Handlers {

// GET /api/insights → AI insights (proxied from GAS via ESP32)
inline void handleInsights() {
  if (!requireAuth()) return;

  String gasResponse = AI::advisor.fetchInsights();

  DynamicJsonDocument gasDoc(8192);
  DeserializationError err = deserializeJson(gasDoc, gasResponse);
  if (err) {
    Serial.printf("[Insights] GAS response parse error: %s\n", err.c_str());
    String mockData = "{\"insights\":[{\"id\":\"mock-parse-error\",\"category\":\"habit_analysis\","
                      "\"severity\":\"info\",\"title\":\"AI Insights temporarily unavailable\","
                      "\"body\":\"GAS returned a malformed response. Relay control unaffected.\",\"channelId\":null,"
                      "\"action\":{\"label\":\"Dismiss\",\"type\":\"dismiss\"},"
                      "\"generatedAt\":0,\"source\":\"mock\",\"advisoryOnly\":true}],"
                      "\"mock\":true,\"error\":\"gas_parse_error\"}";
    sendSuccess("AI insights (mock — GAS response parse error)", mockData);
    return;
  }

  String dataJson;
  dataJson.reserve(4096);
  DynamicJsonDocument outDoc(8192);
  JsonObject data = outDoc.to<JsonObject>();

  if (gasDoc.containsKey("insights")) {
    data["insights"] = gasDoc["insights"];
  } else {
    data.createNestedArray("insights");
  }
  if (gasDoc.containsKey("cached")) data["cached"] = gasDoc["cached"];
  if (gasDoc.containsKey("mock")) data["mock"] = gasDoc["mock"];
  if (gasDoc.containsKey("message")) data["message"] = gasDoc["message"];
  if (gasDoc.containsKey("error")) data["error"] = gasDoc["error"];
  data["gasConfigured"] = AI::advisor.isConfigured();
  data["lastSyncMs"] = (uint32_t)AI::advisor.getLastSyncMs();
  data["lastSyncSuccess"] = AI::advisor.getLastSyncSuccess();

  serializeJson(outDoc, dataJson);
  sendSuccess("AI insights", dataJson);
}

}} // namespace Web::Handlers

#endif
