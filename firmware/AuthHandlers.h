// =============================================================================
// Web/Handlers/AuthHandlers.h — /api/login, /api/logout, /api/session, /api/refresh
// =============================================================================
// R10B-5 (audit round 10B): Refresh token rotation.
// - Login issues access JWT (15min, httpOnly cookie) + refresh token (7day, httpOnly cookie)
// - POST /api/refresh: validates refresh cookie, rotates it, issues new pair
// - Logout: revokes refresh token in NVS + clears both cookies
//
// audit-fixes:
//   - Refresh token is NO LONGER returned in the JSON response body. It is set
//     ONLY as an HttpOnly + Secure + SameSite=Strict cookie. Returning it in
//     JSON defeated the purpose of HttpOnly (any XSS could steal the
//     long-lived 7-day session token). Response now returns only the short-lived
//     access token + csrfToken + expiresAt for client state tracking.
//   - All auth cookies now carry the `Secure` flag when PRODUCTION_BUILD is
//     defined (forces HTTPS-only transport in production).
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

// audit-fixes: cookie suffix applied to all auth cookies.
// In production (PRODUCTION_BUILD defined), cookies are HTTPS-only via `; Secure`.
// In development, omit Secure so cookies work over plain HTTP (e.g., localhost).
#ifdef PRODUCTION_BUILD
static const char* COOKIE_SECURE_SUFFIX = "; Secure";
#else
static const char* COOKIE_SECURE_SUFFIX = "";
#endif

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
// → { token, csrfToken, expiresAt, username }
// audit-fixes: refreshToken NO LONGER returned in JSON. Only HttpOnly cookie.
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
  // audit-fixes: also rate-limit /api/login password brute-force.
  // login() now records failures to the same rate limiter used by checkAuth().
  // IPAddress has implicit conversion to uint32_t, but we make it explicit
  // for clarity (matches the pattern in AuthManager::checkAuth).
  IPAddress clientIp = Web::http.client().remoteIP();
  uint32_t ip = clientIp;
  if (!Services::auth.login(String(user), String(pass),
                             accessToken, refreshToken, csrf, exp, ip)) {
    sendError(401, "Invalid username or password");
    return;
  }

  // Set THREE cookies:
  // - timer12_jwt (access, 15min, HttpOnly + Secure in production)
  // - timer12_refresh (refresh, 7day, HttpOnly + Secure in production) — NOT in JSON
  // - timer12_csrf (CSRF, 15min, readable by JS for double-submit)
  String jwtCookie = "timer12_jwt=";
  jwtCookie += accessToken;
  jwtCookie += "; Path=/; Max-Age=";
  jwtCookie += Core::JWT_ACCESS_TTL_SECONDS;
  jwtCookie += "; SameSite=Strict; HttpOnly";
  jwtCookie += COOKIE_SECURE_SUFFIX;

  String refreshCookie = "timer12_refresh=";
  refreshCookie += refreshToken;
  refreshCookie += "; Path=/; Max-Age=";
  refreshCookie += Core::JWT_REFRESH_TTL_SECONDS;
  refreshCookie += "; SameSite=Strict; HttpOnly";
  refreshCookie += COOKIE_SECURE_SUFFIX;  // audit-fixes: Secure flag

  String csrfCookie = "timer12_csrf=";
  csrfCookie += csrf;
  csrfCookie += "; Path=/; Max-Age=";
  csrfCookie += Core::JWT_ACCESS_TTL_SECONDS;
  csrfCookie += "; SameSite=Strict";
  csrfCookie += COOKIE_SECURE_SUFFIX;  // audit-fixes: Secure flag

  Web::http.sendHeader("Set-Cookie", jwtCookie);
  Web::http.sendHeader("Set-Cookie", refreshCookie, false);  // append
  Web::http.sendHeader("Set-Cookie", csrfCookie, false);     // append

  // audit-fixes: response no longer includes refreshToken.
  // Client must rely on the cookie (auto-sent on every request).
  String data = "{\"token\":\"";
  data += accessToken;
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
  jwtCookie += COOKIE_SECURE_SUFFIX;

  String refreshCookie = "timer12_refresh=";
  refreshCookie += newRefreshToken;
  refreshCookie += "; Path=/; Max-Age=";
  refreshCookie += Core::JWT_REFRESH_TTL_SECONDS;
  refreshCookie += "; SameSite=Strict; HttpOnly";
  refreshCookie += COOKIE_SECURE_SUFFIX;

  String csrfCookie = "timer12_csrf=";
  csrfCookie += csrf;
  csrfCookie += "; Path=/; Max-Age=";
  csrfCookie += Core::JWT_ACCESS_TTL_SECONDS;
  csrfCookie += "; SameSite=Strict";
  csrfCookie += COOKIE_SECURE_SUFFIX;

  Web::http.sendHeader("Set-Cookie", jwtCookie);
  Web::http.sendHeader("Set-Cookie", refreshCookie, false);
  Web::http.sendHeader("Set-Cookie", csrfCookie, false);

  // audit-fixes: response no longer includes refreshToken.
  String data = "{\"token\":\"";
  data += newAccessToken;
  data += "\",\"csrfToken\":\"";
  data += csrf;
  data += "\",\"expiresAt\":";
  data += String((unsigned long)exp * 1000UL);
  data += "}";
  sendSuccess("Tokens refreshed", data);
}

// POST /api/logout
// R10B-5: now revokes refresh token in NVS (true session termination)
// audit-fixes: added CSRF check (logout CSRF is a known vector).
inline void handleLogout() {
  if (!requireCsrf()) return;
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
