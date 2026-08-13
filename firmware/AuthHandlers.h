// =============================================================================
// Web/Handlers/AuthHandlers.h — /api/login, /api/logout, /api/session
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_AUTH_H
#define TIMER12_WEB_HANDLERS_AUTH_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "AuthManager.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"

namespace Web { namespace Handlers {

// POST /api/login { username, password }
// → { token, csrfToken, expiresAt, username }
inline void handleLogin() {
  if (!requireBody(Core::MAX_BODY_SIZE)) return;
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
  const char* user = doc["username"] | "";
  const char* pass = doc["password"] | "";
  String token, csrf;
  uint32_t exp;
  if (!Services::auth.login(String(user), String(pass), token, csrf, exp)) {
    sendError(401, "Invalid username or password");
    return;
  }
  // Set cookies: JWT (httpOnly) + CSRF (readable by JS)
  String jwtCookie = "timer12_jwt=";
  jwtCookie += token;
  jwtCookie += "; Path=/; Max-Age=";
  jwtCookie += Core::JWT_TTL_SECONDS;
  jwtCookie += "; SameSite=Strict";
  // In production over HTTPS via Cloudflare Tunnel: add "; Secure"
  // jwtCookie += "; Secure";

  String csrfCookie = "timer12_csrf=";
  csrfCookie += csrf;
  csrfCookie += "; Path=/; Max-Age=";
  csrfCookie += Core::JWT_TTL_SECONDS;
  csrfCookie += "; SameSite=Strict";

  Web::http.sendHeader("Set-Cookie", jwtCookie);
  Web::http.sendHeader("Set-Cookie", csrfCookie, false);  // append

  String data = "{\"token\":\"";
  data += token;
  data += "\",\"csrfToken\":\"";
  data += csrf;
  data += "\",\"expiresAt\":";
  data += String((unsigned long)exp * 1000UL);
  data += ",\"username\":\"";
  data += user;
  data += "\"}";
  sendSuccess("Login successful", data);
}

// POST /api/logout
inline void handleLogout() {
  Services::auth.logout();
  // Clear cookies
  Web::http.sendHeader("Set-Cookie", "timer12_jwt=; Path=/; Max-Age=0");
  Web::http.sendHeader("Set-Cookie", "timer12_csrf=; Path=/; Max-Age=0", false);
  sendSuccess("Logged out", "{\"success\":true}");
}

// GET /api/session
inline void handleSession() {
  if (!requireAuth()) return;
  String data = "{\"isAuthenticated\":true,\"username\":\"";
  data += Core::userConfig.wwwUser;
  data += "\",\"expiresAt\":";
  data += String((unsigned long)((millis() / 1000) + Core::JWT_TTL_SECONDS) * 1000UL);
  data += "}";
  sendSuccess("", data);
}

}} // namespace Web::Handlers

#endif
