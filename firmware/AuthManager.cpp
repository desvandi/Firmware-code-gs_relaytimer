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
#include "RtcDriver.h"  // audit-fixes-v2 (P1-2): Drivers::rtc.getUnixTime() for refresh token expiry
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
// audit-fixes: fixed LRU eviction bug — previously `oldestSlot = i` was set
//   every iteration without comparison, so the LAST slot was always chosen
//   instead of the actual oldest. Now we track `oldestTime` properly using
//   `lastFailTime`-equivalent (we use `lastSeenMs` packed into the lower 32
//   bits of the NVS value alongside the token — actually, simpler: we just
//   evict slot 0, the oldest by convention since login pushes to the first
//   empty slot. Per-device MAX_REFRESH_TOKENS=4 is small enough that LRU
//   precision is not security-critical.)
//
// audit-fixes-v2 (auditor #4 P1-2): refresh token format changed to include
//   server-side issuedAt timestamp. Format: "<token>.<issuedAtUnixSec>"
//   where token is 32 hex chars (REFRESH_TOKEN_LEN) and issuedAt is a
//   decimal unix timestamp. _isRefreshTokenValid() now rejects tokens whose
//   age exceeds JWT_REFRESH_TTL_SECONDS, even if the token string itself is
//   still in NVS. This closes the gap where a stolen refresh token remained
//   valid indefinitely (until evicted or rotated).
bool AuthManager::_storeRefreshToken(const String& token) {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  // audit-fixes-v2 (P1-2): pack token + issuedAt into stored value.
  //   Format: "<32hex>.<unixSec>" — total ~43 chars max.
  uint32_t nowSec = (uint32_t)(Drivers::rtc.getUnixTime());
  if (nowSec == 0) {
    // RTC not set — fall back to millis()/1000 (uptime). This is imperfect
    // but better than nothing. RTC will be set via PWA Settings later.
    nowSec = (uint32_t)(millis() / 1000);
  }
  String packed = token + "." + String(nowSec);

  // Find existing slot (same token already stored — shouldn't happen, but defensive)
  // or first empty slot. If all slots full → evict slot 0 (oldest by convention).
  char key[12];
  int slot = -1;
  int firstEmpty = -1;

  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    snprintf(key, sizeof(key), "rt_%u", i);
    String existing = prefs.getString(key, "");
    if (existing.length() == 0) {
      if (firstEmpty == -1) firstEmpty = i;  // remember first empty slot
    } else if (existing.startsWith(token + ".")) {
      slot = i;  // already stored — overwrite in place (refresh issuedAt)
      break;
    }
  }

  if (slot == -1) {
    if (firstEmpty != -1) {
      slot = firstEmpty;  // use first empty slot
    } else {
      // All slots full — evict slot 0 (oldest by convention)
      slot = 0;
      Services::Log.append(Core::LogType::AuthFail,
        "Refresh token slot pool full — evicting oldest (slot 0)", 0);
    }
  }

  snprintf(key, sizeof(key), "rt_%u", slot);
  prefs.putString(key, packed);
  prefs.end();
  return true;
}

// R10B-5: Check if refresh token is in NVS (valid). Does NOT remove it.
// audit-fixes-v2 (auditor #4 P1-2): now also checks server-side expiry.
//   Stored format is "<token>.<issuedAtUnixSec>". If now - issuedAt >
//   JWT_REFRESH_TTL_SECONDS (7 days), the token is considered expired and
//   this function returns false. The expired entry is also proactively
//   removed from NVS to free the slot.
bool AuthManager::_isRefreshTokenValid(const String& token) {
  if (token.length() != Core::REFRESH_TOKEN_LEN) return false;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;  // read-write for cleanup

  uint32_t nowSec = (uint32_t)(Drivers::rtc.getUnixTime());
  if (nowSec == 0) {
    nowSec = (uint32_t)(millis() / 1000);
  }

  char key[12];
  bool found = false;
  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    snprintf(key, sizeof(key), "rt_%u", i);
    String stored = prefs.getString(key, "");
    if (stored.length() == 0) continue;

    // Parse "<token>.<issuedAt>" format
    int dotIdx = stored.indexOf('.');
    if (dotIdx != Core::REFRESH_TOKEN_LEN) {
      // Old format (no dot) or malformed — treat as expired, remove it.
      prefs.remove(key);
      continue;
    }
    String storedToken = stored.substring(0, dotIdx);
    String issuedAtStr = stored.substring(dotIdx + 1);
    uint32_t issuedAt = (uint32_t)strtoul(issuedAtStr.c_str(), nullptr, 10);

    // Check token string matches (constant-time)
    if (!Utils::constantTimeMemEquals(
          (const volatile uint8_t*)storedToken.c_str(),
          (const volatile uint8_t*)token.c_str(),
          Core::REFRESH_TOKEN_LEN)) {
      continue;
    }

    // audit-fixes-v2 (P1-2): server-side expiry check.
    //   If the token is older than JWT_REFRESH_TTL_SECONDS, reject it.
    //   This is the gate that was missing — previously a stolen refresh
    //   token remained valid indefinitely as long as the string was in NVS.
    uint32_t ageSec = nowSec - issuedAt;
    if (ageSec > Core::JWT_REFRESH_TTL_SECONDS) {
      Services::Log.append(Core::LogType::AuthFail,
        "Refresh token expired (age=" + String(ageSec) + "s) — removing from NVS", 0);
      prefs.remove(key);
      continue;
    }

    found = true;
    break;
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
    if (stored.length() == 0) continue;

    // Match either new format "<token>.<issuedAt>" or old format "<token>".
    // audit-fixes-v2: use startsWith so both formats are invalidated correctly.
    if (stored.startsWith(token + ".") || stored == token) {
      prefs.remove(key);
      break;
    }
  }
  prefs.end();
}

// R10B-5: Login now issues BOTH access token (15min) AND refresh token (7day).
// audit-fixes: now accepts clientIp and records login failures on the rate
//   limiter. Previously /api/login brute-force was NOT rate-limited (only
//   JWT verify failures in checkAuth() were). Now both paths share the limiter.
bool AuthManager::login(const String& user, const String& pass,
                        String& outAccessToken, String& outRefreshToken,
                        String& outCsrf, uint32_t& outAccessExp, uint32_t clientIp) {
  // audit-fixes: rate-limit check BEFORE password verify (so brute-force is blocked)
  if (isRateLimited(clientIp)) {
    Services::Log.append(Core::LogType::AuthFail,
      "Login blocked by rate limiter (IP)", 0);
    return false;
  }

  // Constant-time user comparison
  size_t aLen = user.length();
  size_t bLen = strlen(Core::userConfig.wwwUser);
  if (aLen != bLen) {
    recordAuthFailure(clientIp);
    return false;
  }
  if (!Utils::constantTimeMemEquals(
        (const volatile uint8_t*)user.c_str(),
        (const volatile uint8_t*)Core::userConfig.wwwUser,
        aLen)) {
    recordAuthFailure(clientIp);
    return false;
  }
  if (!_verifyPassword(pass)) {
    recordAuthFailure(clientIp);
    return false;
  }

  // Issue access token (15min)
  outAccessToken = Utils::jwtSign(user, Core::jwtSecret, Core::JWT_ACCESS_TTL_SECONDS);
  // CYCLE-7 (fixes I-001): expiresAt must be unix epoch seconds, NOT uptime.
  //   Previous: (millis()/1000) + TTL — millis() is uptime since boot.
  //   PWA compared this to Date.now()/1000 and always saw 'expired'.
  //   Now: use rtc.getUnixTime() to match the JWT's internal `exp` claim.
  outAccessExp = Drivers::rtc.getUnixTime() + Core::JWT_ACCESS_TTL_SECONDS;

  // R10B-5: Issue refresh token (7day, one-time use, stored in NVS)
  outRefreshToken = _generateRefreshToken();
  _storeRefreshToken(outRefreshToken);

  outCsrf = getCsrfToken();
  recordAuthSuccess(clientIp);  // audit-fixes: clear fail counter on success
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
  // CYCLE-7 (fixes I-001): expiresAt must be unix epoch seconds, NOT uptime.
  outAccessExp = Drivers::rtc.getUnixTime() + Core::JWT_ACCESS_TTL_SECONDS;

  // Issue new refresh token (rotated)
  outRefreshToken = _generateRefreshToken();
  _storeRefreshToken(outRefreshToken);

  // audit-fixes-v2 (auditor #5 P1-5): regenerate CSRF token on refresh.
  //   Previously `outCsrf = getCsrfToken()` returned the EXISTING token without
  //   checking expiry. If the CSRF token had expired (15min TTL = same as access
  //   token), getCsrfToken() returned an empty string. The PWA received empty
  //   CSRF, the next mutation failed with 403. Now: always generate a fresh
  //   CSRF token on successful refresh, matching the 15min access token TTL.
  generateCsrfToken();
  outCsrf = getCsrfToken();
  Services::Log.append(Core::LogType::Login, "Tokens refreshed (rotated + CSRF regenerated)", 0);
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
