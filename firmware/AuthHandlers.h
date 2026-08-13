// =============================================================================
// Web/Handlers/AuthHandlers.h — /api/login, /api/logout, /api/session, /api/refresh
// =============================================================================
// R10B-5 (audit round 10B): Refresh token rotation.
// - Login issues access JWT (15min, httpOnly cookie) + refresh token (7day, httpOnly cookie)
// - POST /api/refresh: validates refresh cookie, rotates it, issues new pair
// - Logout: revokes refresh token in NVS + clears both cookies
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

// Helper: extract refresh token from Cookie header
inline String extractRefreshTokenFromCookie() {
  if (!Web::http.hasHeader("Cookie")) return "";
  String cookie = Web::http.header("Cookie");
  int idx = cookie.indexOf("timer12_refresh=");
  if (idx < 0) return "";
  int start = idx + 16;
  int end = cookie.indexOf(';', start);
  if (end < 0) end = cookie.length();
  return cookie.substring(start, end);
}

// POST /api/login { username, password }
// → { token, refreshToken, csrfToken, expiresAt, username }
// R10B-5: now also issues refresh token (7day, httpOnly cookie)
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
  String accessToken, refreshToken, csrf;
  uint32_t exp;
  if (!Services::auth.login(String(user), String(pass),
                             accessToken, refreshToken, csrf, exp)) {
    sendError(401, "Invalid username or password");
    return;
  }

  // R10B-5: Set THREE cookies:
  // - timer12_jwt (access, 15min, httpOnly)
  // - timer12_refresh (refresh, 7day, httpOnly) ← NEW
  // - timer12_csrf (CSRF, 15min, readable by JS)
  String jwtCookie = "timer12_jwt=";
  jwtCookie += accessToken;
  jwtCookie += "; Path=/; Max-Age=";
  jwtCookie += Core::JWT_ACCESS_TTL_SECONDS;
  jwtCookie += "; SameSite=Strict; HttpOnly";

  String refreshCookie = "timer12_refresh=";
  refreshCookie += refreshToken;
  refreshCookie += "; Path=/; Max-Age=";
  refreshCookie += Core::JWT_REFRESH_TTL_SECONDS;
  refreshCookie += "; SameSite=Strict; HttpOnly";  // httpOnly — JS can't read

  String csrfCookie = "timer12_csrf=";
  csrfCookie += csrf;
  csrfCookie += "; Path=/; Max-Age=";
  csrfCookie += Core::JWT_ACCESS_TTL_SECONDS;
  csrfCookie += "; SameSite=Strict";

  Web::http.sendHeader("Set-Cookie", jwtCookie);
  Web::http.sendHeader("Set-Cookie", refreshCookie, false);  // append
  Web::http.sendHeader("Set-Cookie", csrfCookie, false);     // append

  String data = "{\"token\":\"";
  data += accessToken;
  data += "\",\"refreshToken\":\"";
  data += refreshToken;
  data += "\",\"csrfToken\":\"";
  data += csrf;
  data += "\",\"expiresAt\":";
  data += String((unsigned long)exp * 1000UL);
  data += ",\"username\":\"";
  data += user;
  data += "\"}";
  sendSuccess("Login successful", data);
}

// POST /api/refresh
// R10B-5: Validates refresh token from cookie, rotates it (old invalidated),
// issues new access + refresh token pair. No body needed — reads from cookie.
inline void handleRefresh() {
  String oldRefreshToken = extractRefreshTokenFromCookie();
  if (oldRefreshToken.length() == 0) {
    sendError(401, "No refresh token — please login");
    return;
  }

  String newAccessToken, newRefreshToken, csrf;
  uint32_t exp;
  if (!Services::auth.refreshTokens(oldRefreshToken,
                                     newAccessToken, newRefreshToken, csrf, exp)) {
    // Refresh token invalid/reused → clear cookies, force re-login
    Web::http.sendHeader("Set-Cookie", "timer12_jwt=; Path=/; Max-Age=0");
    Web::http.sendHeader("Set-Cookie", "timer12_refresh=; Path=/; Max-Age=0", false);
    Web::http.sendHeader("Set-Cookie", "timer12_csrf=; Path=/; Max-Age=0", false);
    sendError(401, "Refresh token invalid or reused — please login again");
    return;
  }

  // Set new cookies (rotated)
  String jwtCookie = "timer12_jwt=";
  jwtCookie += newAccessToken;
  jwtCookie += "; Path=/; Max-Age=";
  jwtCookie += Core::JWT_ACCESS_TTL_SECONDS;
  jwtCookie += "; SameSite=Strict; HttpOnly";

  String refreshCookie = "timer12_refresh=";
  refreshCookie += newRefreshToken;
  refreshCookie += "; Path=/; Max-Age=";
  refreshCookie += Core::JWT_REFRESH_TTL_SECONDS;
  refreshCookie += "; SameSite=Strict; HttpOnly";

  String csrfCookie = "timer12_csrf=";
  csrfCookie += csrf;
  csrfCookie += "; Path=/; Max-Age=";
  csrfCookie += Core::JWT_ACCESS_TTL_SECONDS;
  csrfCookie += "; SameSite=Strict";

  Web::http.sendHeader("Set-Cookie", jwtCookie);
  Web::http.sendHeader("Set-Cookie", refreshCookie, false);
  Web::http.sendHeader("Set-Cookie", csrfCookie, false);

  String data = "{\"token\":\"";
  data += newAccessToken;
  data += "\",\"refreshToken\":\"";
  data += newRefreshToken;
  data += "\",\"csrfToken\":\"";
  data += csrf;
  data += "\",\"expiresAt\":";
  data += String((unsigned long)exp * 1000UL);
  data += "}";
  sendSuccess("Tokens refreshed", data);
}

// POST /api/logout
// R10B-5: now revokes refresh token in NVS (true session termination)
inline void handleLogout() {
  Services::auth.logout(Web::http);
  // Clear all three cookies
  Web::http.sendHeader("Set-Cookie", "timer12_jwt=; Path=/; Max-Age=0");
  Web::http.sendHeader("Set-Cookie", "timer12_refresh=; Path=/; Max-Age=0", false);
  Web::http.sendHeader("Set-Cookie", "timer12_csrf=; Path=/; Max-Age=0", false);
  sendSuccess("Logged out", "{\"success\":true}");
}

// GET /api/session
inline void handleSession() {
  if (!requireAuth()) return;
  String data = "{\"isAuthenticated\":true,\"username\":\"";
  data += Core::userConfig.wwwUser;
  data += "\",\"expiresAt\":";
  data += String((unsigned long)((millis() / 1000) + Core::JWT_ACCESS_TTL_SECONDS) * 1000UL);
  data += "}";
  sendSuccess("", data);
}

}} // namespace Web::Handlers

#endif
