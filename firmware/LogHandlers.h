// =============================================================================
// Web/Handlers/LogHandlers.h — /api/log, /api/audit_log
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_LOG_H
#define TIMER12_WEB_HANDLERS_LOG_H

#include <Arduino.h>
#include "Common.h"
#include "AuthManager.h"
#include "LogService.h"
#include "Config.h"

namespace Web { namespace Handlers {

// GET /api/log?type=X&channelId=Y&limit=N
inline void handleGetLogs() {
  if (!requireAuth()) return;
  int filterType = -1;
  if (Web::http.hasArg("type")) {
    String t = Web::http.arg("type");
    static const char* names[] = {
      "relay_on", "relay_off", "pir_trigger", "login", "logout",
      "error", "restart", "ota", "config_change", "factory_reset",
      "time_sync", "auth_fail"
    };
    for (int i = 0; i < 12; i++) {
      if (t == names[i]) { filterType = i; break; }
    }
  }
  int filterChannel = 0;
  if (Web::http.hasArg("channelId")) {
    filterChannel = Web::http.arg("channelId").toInt();
  }
  int limit = 200;
  if (Web::http.hasArg("limit")) {
    limit = Web::http.arg("limit").toInt();
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;
  }
  String json = Services::Log.getActivityLogJson(limit, filterType, filterChannel);
  // audit-fixes: was `Access-Control-Allow-Origin: *` hardcoded — now uses
  //   shared helper so production builds enforce configured origin list.
  String origin = getAllowedOrigin();
  Web::http.sendHeader("X-Frame-Options", "DENY");
  Web::http.sendHeader("Cache-Control", "no-store");
  if (origin.length() > 0) {
    Web::http.sendHeader("Access-Control-Allow-Origin", origin);
    Web::http.sendHeader("Access-Control-Allow-Credentials", "true");
    if (origin != "*") Web::http.sendHeader("Vary", "Origin");
  }
  // Already contains success+data envelope
  Web::http.send(200, "application/json; charset=utf-8", json);
}

// GET /api/audit_log → plain-text audit log (forensic)
inline void handleGetAuditLog() {
  if (!requireAuth()) return;
  String txt = Services::Log.getAuditLogText();
  Web::http.sendHeader("X-Frame-Options", "DENY");
  Web::http.sendHeader("Content-Type", "text/plain; charset=utf-8");
  Web::http.send(200, "text/plain; charset=utf-8", txt);
}

}} // namespace Web::Handlers

#endif
