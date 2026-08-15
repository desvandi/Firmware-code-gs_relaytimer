// =============================================================================
// Web/Handlers/Common.h — Shared helpers used by all handlers
// =============================================================================
// P0 #10 (audit round 9): CORS origin now configurable via Config::ALLOWED_CORS_ORIGINS.
// Default: "*" (any origin — development only).
// Production: set to PWA's Vercel URL, e.g., "https://remote-relay.vercel.app".
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_COMMON_H
#define TIMER12_WEB_HANDLERS_COMMON_H

#include <Arduino.h>
#include <WebServer.h>
#include "AuthManager.h"
#include "HttpServer.h"
#include "Config.h"

namespace Web {

// Determine the correct Access-Control-Allow-Origin header value for the
// incoming request. If Config::ALLOWED_CORS_ORIGINS is "*", returns "*".
// Otherwise, checks if the request's Origin header matches one of the
// comma-separated allowed origins and returns it (echo-back pattern for
// credentialed CORS). Returns empty string if origin not allowed.
inline String getAllowedOrigin() {
  if (strcmp(Core::ALLOWED_CORS_ORIGINS, "*") == 0) {
    return "*";
  }
  // Check Origin header against allowed list
  if (Web::http.hasHeader("Origin")) {
    String origin = Web::http.header("Origin");
    // Parse comma-separated list
    String allowed = Core::ALLOWED_CORS_ORIGINS;
    int start = 0;
    while (start < (int)allowed.length()) {
      int comma = allowed.indexOf(',', start);
      String one = (comma < 0) ? allowed.substring(start) : allowed.substring(start, comma);
      one.trim();
      if (one.length() > 0 && origin == one) {
        return origin;  // echo back the specific origin
      }
      if (comma < 0) break;
      start = comma + 1;
    }
  }
  return "";  // origin not allowed
}

// Send CORS + security headers (used by all JSON responses)
inline void sendSecurityHeaders() {
  String origin = getAllowedOrigin();
  if (origin.length() > 0) {
    Web::http.sendHeader("Access-Control-Allow-Origin", origin);
    Web::http.sendHeader("Access-Control-Allow-Credentials", "true");
    Web::http.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    Web::http.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-CSRF-Token");
    if (origin != "*") {
      Web::http.sendHeader("Vary", "Origin");
    }
  }
  Web::http.sendHeader("X-Frame-Options", "DENY");
  Web::http.sendHeader("X-Content-Type-Options", "nosniff");
  Web::http.sendHeader("Cache-Control", "no-store");
  Web::http.sendHeader("Referrer-Policy", "no-referrer");
}

// Send success envelope: { success: true, message, data }
inline void sendSuccess(const String& message, const String& dataJson = "{}") {
  String body = "{\"success\":true,\"message\":\"";
  body += message;
  body += "\",\"data\":";
  body += dataJson;
  body += "}";
  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", body);
}

// Send error envelope: { success: false, message, data: null }
inline void sendError(int code, const String& message) {
  String body = "{\"success\":false,\"message\":\"";
  body += message;
  body += "\",\"data\":null}";
  sendSecurityHeaders();
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

// P2-2 F-P0-2 C2: requestId validation (charset + length).
//
// Rules (mirror MqttClient.cpp _handleCommand lines 876-898 for cross-ingress
// contract symmetry):
//   - Must be present (non-empty)
//   - Max 64 chars
//   - Only [a-zA-Z0-9-_]
//
// Returns true if valid. On false, sends HTTP 400 to client.
//
// This is the REST-side analog of MQTT's requestId requirement — both
// paths now reject commands without a valid requestId.
inline bool validateRequestId(const String& requestId) {
  if (requestId.length() == 0) {
    sendError(400, "requestId required");
    return false;
  }
  if (requestId.length() > 64) {
    sendError(400, "requestId too long (max 64 chars)");
    return false;
  }
  for (size_t i = 0; i < requestId.length(); i++) {
    char c = requestId[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      sendError(400, "requestId contains invalid characters (use UUID format: [a-zA-Z0-9-_])");
      return false;
    }
  }
  return true;
}

// P2-2 F-P0-2 C2: Extract requestId from JSON body and validate.
//
// Convenience wrapper around Web::validateRequestId(). Extracts the
// "requestId" field from the parsed JSON document, validates charset/length
// per the cross-ingress contract (mirrors MqttClient.cpp lines 876-898),
// and stores it in `out`.
//
// Returns true if valid. On false, sends HTTP 400 to client.
//
// Pre-condition: `doc` has been parsed from the request body via
//                deserializeJson(doc, Web::http.arg("plain")).
// Post-condition on success: `out` contains the validated requestId string.
inline bool requireRequestId(const DynamicJsonDocument& doc, String& out) {
  const char* rid = doc["requestId"] | "";
  out = String(rid);
  return validateRequestId(out);
}

} // namespace Web

#endif
