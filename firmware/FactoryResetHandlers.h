// =============================================================================
// Web/Handlers/FactoryResetHandlers.h — /api/factory_reset/prepare, /confirm
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_FACTORY_RESET_H
#define TIMER12_WEB_HANDLERS_FACTORY_RESET_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "AuthManager.h"
#include "ConfigStore.h"
#include "LogService.h"
#include "RelayDriver.h"
#include "PirDriver.h"
#include "Config.h"
#include "Globals.h"

namespace Web { namespace Handlers {

// POST /api/factory_reset/prepare → token (valid 60s)
inline void handleFactoryResetPrepare() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  String token = Services::auth.generateFactoryResetToken();
  uint32_t exp = (uint32_t)(millis() / 1000) + (Core::FACTORY_RESET_TOKEN_TTL_MS / 1000);
  String data = "{\"token\":\"";
  data += token;
  data += "\",\"expiresAt\":";
  data += String((unsigned long)exp * 1000UL);
  data += "}";
  sendSuccess("Reset token generated (valid 60s)", data);
}

// POST /api/factory_reset/confirm { token, confirm: "RESET" }
inline void handleFactoryResetConfirm() {
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
  const char* token = doc["token"] | "";
  const char* confirm = doc["confirm"] | "";
  if (strcmp(confirm, "RESET") != 0) {
    sendError(400, "Confirmation must be \"RESET\"");
    return;
  }
  if (!Services::auth.consumeFactoryResetToken(String(token))) {
    sendError(403, "Invalid or expired reset token");
    return;
  }
  // Perform reset
  Drivers::relay.allOff();
  Drivers::pir.resetAll();
  Storage::config.resetChannels();
  Storage::config.saveSchedule(true);
  Storage::config.initDefaultUserConfig();
  Storage::config.saveUserConfig();
  Services::Log.append(Core::LogType::FactoryReset, "Factory reset performed", 0);
  sendSuccess("Factory reset complete. System rebooting.", "{\"reset\":true}");
  delay(500);
  ESP.restart();
}

}} // namespace Web::Handlers

#endif
