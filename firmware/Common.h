// =============================================================================
// Web/Handlers/Common.h — Shared helpers used by all handlers
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_COMMON_H
#define TIMER12_WEB_HANDLERS_COMMON_H

#include <Arduino.h>
#include <WebServer.h>
#include "AuthManager.h"
#include "HttpServer.h"

namespace Web {

// Send success envelope: { success: true, message, data }
inline void sendSuccess(const String& message, const String& dataJson = "{}") {
  String body = "{\"success\":true,\"message\":\"";
  body += message;
  body += "\",\"data\":";
  body += dataJson;
  body += "}";
  Web::http.sendHeader("X-Frame-Options", "DENY");
  Web::http.sendHeader("X-Content-Type-Options", "nosniff");
  Web::http.sendHeader("Cache-Control", "no-store");
  Web::http.sendHeader("Access-Control-Allow-Origin", "*");
  Web::http.sendHeader("Access-Control-Allow-Credentials", "true");
  Web::http.send(200, "application/json; charset=utf-8", body);
}

// Send error envelope: { success: false, message, data: null }
inline void sendError(int code, const String& message) {
  String body = "{\"success\":false,\"message\":\"";
  body += message;
  body += "\",\"data\":null}";
  Web::http.sendHeader("X-Frame-Options", "DENY");
  Web::http.sendHeader("X-Content-Type-Options", "nosniff");
  Web::http.sendHeader("Access-Control-Allow-Origin", "*");
  Web::http.sendHeader("Access-Control-Allow-Credentials", "true");
  Web::http.send(code, "application/json; charset=utf-8", body);
}

inline bool requireAuth() {
  return Services::auth.checkAuth(Web::http);
}

inline bool requireCsrf() {
  if (!Services::auth.checkCsrfToken(Web::http)) {
    sendError(403, "Invalid CSRF token");
    return false;
  }
  return true;
}

inline bool requireBody(size_t maxSize) {
  if (Web::http.hasHeader("Content-Length")) {
    size_t len = (size_t)Web::http.header("Content-Length").toInt();
    if (len > maxSize) {
      sendError(413, "Body too large");
      return false;
    }
  }
  return true;
}

} // namespace Web

#endif
