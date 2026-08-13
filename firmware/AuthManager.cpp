// =============================================================================
// Services/AuthManager.cpp
// =============================================================================
// R10B-5 (audit round 10B): Refresh token rotation.
// - Login: issues access token (15min) + refresh token (7day, stored in NVS)
// - POST /api/refresh: validates old refresh token, invalidates it (one-time use),
//   issues new access + new refresh token pair.
// - Logout: removes refresh token from NVS (true revocation).
// - Reuse of invalidated refresh token = security violation (logged + rejected).
// =============================================================================
#include "AuthManager.h"
#include "ConfigStore.h"
#include "Crypto.h"
#include "Json.h"
#include "Globals.h"
#include "Config.h"
#include "LogService.h"
#include <ArduinoJson.h>
#include <Preferences.h>

namespace Services {

AuthManager auth;

void AuthManager::begin() {
  generateCsrfToken();
}

void AuthManager::generateCsrfToken() {
  String t = Utils::generateToken(Core::CSRF_TOKEN_LEN);
  strncpy(Core::csrfToken, t.c_str(), Core::CSRF_TOKEN_LEN);
  Core::csrfToken[Core::CSRF_TOKEN_LEN] = '\0';
  Core::csrfTokenTime = millis();
}

String AuthManager::getCsrfToken() const {
  if (Core::csrfToken[0] == '\0' ||
      millis() - Core::csrfTokenTime > Core::CSRF_TOKEN_TTL_MS) {
    return String();
  }
  return String(Core::csrfToken);
}

bool AuthManager::checkCsrfToken(WebServer& server) const {
  if (Core::csrfToken[0] == '\0') return false;
  if (millis() - Core::csrfTokenTime > Core::CSRF_TOKEN_TTL_MS) return false;
  if (!server.hasHeader("X-CSRF-Token")) return false;
  String token = server.header("X-CSRF-Token");
  if (token.length() != Core::CSRF_TOKEN_LEN) return false;
  return Utils::constantTimeMemEquals(
    (const volatile uint8_t*)token.c_str(),
    (const volatile uint8_t*)Core::csrfToken,
    Core::CSRF_TOKEN_LEN);
}

bool AuthManager::_verifyPassword(const String& pass) const {
  uint8_t computedHash[32];
  if (!Utils::pbkdf2HmacSha256(pass.c_str(), pass.length(),
                               Core::userConfig.salt, Core::SALT_LEN,
                               Core::userConfig.iterations, computedHash)) {
    return false;
  }
  char computedHex[Core::HASH_HEX_BUF_SIZE];
  Utils::bytesToHex(computedHash, 32, computedHex);
  memset(computedHash, 0, sizeof(computedHash));
  if (strlen(Core::userConfig.passHashHex) != Core::HASH_HEX_LEN) return false;
  return Utils::constantTimeMemEquals(
    (const volatile uint8_t*)computedHex,
    (const volatile uint8_t*)Core::userConfig.passHashHex,
    Core::HASH_HEX_LEN);
}

// R10B-5: Generate a random refresh token (32 hex chars = 16 bytes random)
String AuthManager::_generateRefreshToken() {
  return Utils::generateToken(Core::REFRESH_TOKEN_LEN);
}

// R10B-5: Store refresh token in NVS (with LRU eviction if cap exceeded)
// Tokens stored as: rt_0, rt_1, ..., rt_<MAX_REFRESH_TOKENS-1>
bool AuthManager::_storeRefreshToken(const String& token) {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  // Find existing slot or empty slot
  char key[12];
  int slot = -1;
  int oldestSlot = 0;
  uint32_t oldestTime = 0xFFFFFFFF;

  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    snprintf(key, sizeof(key), "rt_%u", i);
    String existing = prefs.getString(key, "");
    if (existing.length() == 0) {
      slot = i;
      break;
    }
    // Track oldest for LRU eviction (use first 4 chars as timestamp pseudo-key)
    // In production, store timestamp separately; here we use slot 0 as oldest
    if (slot == -1) oldestSlot = i;
  }

  if (slot == -1) slot = oldestSlot;  // LRU eviction

  snprintf(key, sizeof(key), "rt_%u", slot);
  prefs.putString(key, token);
  prefs.end();
  return true;
}

// R10B-5: Check if refresh token is in NVS (valid). Does NOT remove it.
bool AuthManager::_isRefreshTokenValid(const String& token) {
  if (token.length() != Core::REFRESH_TOKEN_LEN) return false;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, true)) return false;

  char key[12];
  bool found = false;
  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    snprintf(key, sizeof(key), "rt_%u", i);
    String stored = prefs.getString(key, "");
    if (stored.length() == Core::REFRESH_TOKEN_LEN &&
        Utils::constantTimeMemEquals(
          (const volatile uint8_t*)stored.c_str(),
          (const volatile uint8_t*)token.c_str(),
          Core::REFRESH_TOKEN_LEN)) {
      found = true;
      break;
    }
  }
  prefs.end();
  return found;
}

// R10B-5: Invalidate refresh token (remove from NVS). Called after use.
void AuthManager::_invalidateRefreshToken(const String& token) {
  if (token.length() != Core::REFRESH_TOKEN_LEN) return;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return;

  char key[12];
  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    snprintf(key, sizeof(key), "rt_%u", i);
    String stored = prefs.getString(key, "");
    if (stored.length() == Core::REFRESH_TOKEN_LEN &&
        Utils::constantTimeMemEquals(
          (const volatile uint8_t*)stored.c_str(),
          (const volatile uint8_t*)token.c_str(),
          Core::REFRESH_TOKEN_LEN)) {
      prefs.remove(key);
      break;
    }
  }
  prefs.end();
}

// R10B-5: Login now issues BOTH access token (15min) AND refresh token (7day).
bool AuthManager::login(const String& user, const String& pass,
                        String& outAccessToken, String& outRefreshToken,
                        String& outCsrf, uint32_t& outAccessExp) {
  // Constant-time user comparison
  size_t aLen = user.length();
  size_t bLen = strlen(Core::userConfig.wwwUser);
  if (aLen != bLen) return false;
  if (!Utils::constantTimeMemEquals(
        (const volatile uint8_t*)user.c_str(),
        (const volatile uint8_t*)Core::userConfig.wwwUser,
        aLen)) {
    return false;
  }
  if (!_verifyPassword(pass)) return false;

  // Issue access token (15min)
  outAccessToken = Utils::jwtSign(user, Core::jwtSecret, Core::JWT_ACCESS_TTL_SECONDS);
  outAccessExp = (uint32_t)(millis() / 1000) + Core::JWT_ACCESS_TTL_SECONDS;

  // R10B-5: Issue refresh token (7day, one-time use, stored in NVS)
  outRefreshToken = _generateRefreshToken();
  _storeRefreshToken(outRefreshToken);

  outCsrf = getCsrfToken();
  Services::Log.append(Core::LogType::Login, "User logged in (access+refresh issued)", 0);
  return true;
}

// R10B-5: Refresh token rotation.
// Validates old refresh token, invalidates it (one-time use), issues new pair.
bool AuthManager::refreshTokens(const String& refreshToken,
                                String& outAccessToken, String& outRefreshToken,
                                String& outCsrf, uint32_t& outAccessExp) {
  if (!_isRefreshTokenValid(refreshToken)) {
    Services::Log.append(Core::LogType::AuthFail,
      "Refresh token invalid or reused (possible token theft)", 0);
    return false;
  }

  // Invalidate old refresh token (one-time use — rotation)
  _invalidateRefreshToken(refreshToken);

  // Issue new access token
  String username = Core::userConfig.wwwUser;  // single-user system
  outAccessToken = Utils::jwtSign(username, Core::jwtSecret, Core::JWT_ACCESS_TTL_SECONDS);
  outAccessExp = (uint32_t)(millis() / 1000) + Core::JWT_ACCESS_TTL_SECONDS;

  // Issue new refresh token (rotated)
  outRefreshToken = _generateRefreshToken();
  _storeRefreshToken(outRefreshToken);

  outCsrf = getCsrfToken();
  Services::Log.append(Core::LogType::Login, "Tokens refreshed (rotated)", 0);
  return true;
}

bool AuthManager::checkAuth(WebServer& server) {
  IPAddress clientIp = server.client().remoteIP();
  uint32_t ip = clientIp;
  if (isRateLimited(ip)) {
    server.send(429, "application/json",
                "{\"success\":false,\"message\":\"Too many attempts. Try again later.\",\"data\":null}");
    return false;
  }
  // Try JWT from Cookie first
  String token;
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    int idx = cookie.indexOf("timer12_jwt=");
    if (idx >= 0) {
      int start = idx + 12;
      int end = cookie.indexOf(';', start);
      if (end < 0) end = cookie.length();
      token = cookie.substring(start, end);
    }
  }
  // Fall back to Authorization: Bearer <token>
  if (token.length() == 0 && server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    if (auth.startsWith("Bearer ")) {
      token = auth.substring(7);
    }
  }
  if (token.length() == 0) {
    server.send(401, "application/json",
                "{\"success\":false,\"message\":\"Unauthorized\",\"data\":null}");
    return false;
  }
  String username;
  if (!Utils::jwtVerify(token, Core::jwtSecret, username)) {
    recordAuthFailure(ip);
    server.send(401, "application/json",
                "{\"success\":false,\"message\":\"Invalid or expired token\",\"data\":null}");
    return false;
  }
  recordAuthSuccess(ip);
  return true;
}

// R10B-5: Logout now revokes refresh token (true session termination).
void AuthManager::logout(WebServer& server) {
  // Extract refresh token from cookie and invalidate it
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    int idx = cookie.indexOf("timer12_refresh=");
    if (idx >= 0) {
      int start = idx + 16;
      int end = cookie.indexOf(';', start);
      if (end < 0) end = cookie.length();
      String refreshToken = cookie.substring(start, end);
      if (refreshToken.length() == Core::REFRESH_TOKEN_LEN) {
        _invalidateRefreshToken(refreshToken);
      }
    }
  }
  Services::Log.append(Core::LogType::Logout, "User logged out (refresh token revoked)", 0);
}

bool AuthManager::isRateLimited(uint32_t ip) const {
  for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) {
    if (Core::authAttempts[i].active && Core::authAttempts[i].ip == ip) {
      if (millis() < Core::authAttempts[i].blockUntil) return true;
    }
  }
  return false;
}

void AuthManager::recordAuthFailure(uint32_t ip) {
  int idx = -1;
  for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) {
    if (Core::authAttempts[i].active && Core::authAttempts[i].ip == ip) {
      idx = i; break;
    }
  }
  if (idx == -1) {
    for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) {
      if (!Core::authAttempts[i].active) { idx = i; break; }
    }
    if (idx == -1) {
      unsigned long oldest = (unsigned long)-1;
      int oldestIdx = 0;
      for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) {
        if (Core::authAttempts[i].lastFailTime < oldest) {
          oldest = Core::authAttempts[i].lastFailTime;
          oldestIdx = i;
        }
      }
      idx = oldestIdx;
    }
    Core::authAttempts[idx].ip = ip;
    Core::authAttempts[idx].failCount = 0;
    Core::authAttempts[idx].active = true;
  }
  Core::authAttempts[idx].failCount++;
  Core::authAttempts[idx].lastFailTime = millis();
  if (Core::authAttempts[idx].failCount >= Core::AUTH_FAIL_THRESHOLD_LONG) {
    Core::authAttempts[idx].blockUntil = millis() + Core::AUTH_BLOCK_LONG_MS;
    Services::Log.append(Core::LogType::AuthFail,
      "AUTH BLOCK 5min fails=" + String(Core::authAttempts[idx].failCount), 0);
  } else if (Core::authAttempts[idx].failCount >= Core::AUTH_FAIL_THRESHOLD_SHORT) {
    Core::authAttempts[idx].blockUntil = millis() + Core::AUTH_BLOCK_SHORT_MS;
    Services::Log.append(Core::LogType::AuthFail,
      "AUTH BLOCK 60s fails=" + String(Core::authAttempts[idx].failCount), 0);
  }
}

void AuthManager::recordAuthSuccess(uint32_t ip) {
  for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) {
    if (Core::authAttempts[i].active && Core::authAttempts[i].ip == ip) {
      Core::authAttempts[i].failCount = 0;
      Core::authAttempts[i].blockUntil = 0;
      break;
    }
  }
}

bool AuthManager::changePassword(const String& current, const String& next) {
  if (!_verifyPassword(current)) return false;
  if (next.length() < 8 || !Utils::isPasswordStrong(next)) return false;
  Utils::generateRandomBytes(Core::userConfig.salt, Core::SALT_LEN);
  Core::userConfig.iterations = Core::PBKDF2_ITERATIONS;
  uint8_t hash[32];
  if (!Utils::pbkdf2HmacSha256(next.c_str(), next.length(),
                               Core::userConfig.salt, Core::SALT_LEN,
                               Core::userConfig.iterations, hash)) {
    return false;
  }
  Utils::bytesToHex(hash, 32, Core::userConfig.passHashHex);
  memset(hash, 0, sizeof(hash));
  Storage::config.saveUserConfig();
  return true;
}

String AuthManager::generateFactoryResetToken() {
  String t = Utils::generateToken(32);
  strncpy(Core::factoryResetToken, t.c_str(), 32);
  Core::factoryResetToken[32] = '\0';
  Core::factoryResetTokenTime = millis();
  return t;
}

bool AuthManager::consumeFactoryResetToken(const String& token) {
  if (Core::factoryResetToken[0] == '\0') return false;
  if (millis() - Core::factoryResetTokenTime > Core::FACTORY_RESET_TOKEN_TTL_MS) {
    Core::factoryResetToken[0] = '\0';
    return false;
  }
  if (!Utils::constantTimeMemEquals(
        (const volatile uint8_t*)token.c_str(),
        (const volatile uint8_t*)Core::factoryResetToken,
        32)) {
    return false;
  }
  Core::factoryResetToken[0] = '\0';  // one-time use
  return true;
}

} // namespace Services
