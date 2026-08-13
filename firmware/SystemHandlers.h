// =============================================================================
// Web/Handlers/SystemHandlers.h — /api/reboot
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_SYSTEM_H
#define TIMER12_WEB_HANDLERS_SYSTEM_H

#include <Arduino.h>
#include "Common.h"
#include "ConfigStore.h"
#include "AuthManager.h"
#include "LogService.h"
#include "Config.h"

namespace Web { namespace Handlers {

// POST /api/reboot
inline void handleReboot() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  sendSuccess("System rebooting", "{\"rebooting\":true}");
  if (Core::scheduleDirty) Storage::config.saveSchedule(true);
  Services::Log.append(Core::LogType::Restart, "Reboot triggered", 0);
  delay(500);
  ESP.restart();
}

}} // namespace Web::Handlers

#endif
