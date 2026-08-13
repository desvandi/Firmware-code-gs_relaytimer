// =============================================================================
// Services/AuthManager.h — JWT + CSRF + rate limiting + refresh token rotation
// =============================================================================
// R10B-5 (audit round 10B): Refresh token rotation implemented.
// Login returns access token (15min) + refresh token (7day, one-time use).
// POST /api/refresh validates old refresh token, invalidates it, issues new
// access + refresh token pair. Reuse of old refresh token = security violation.
#pragma once
#ifndef TIMER12_SERVICES_AUTH_H
#define TIMER12_SERVICES_AUTH_H

#include <Arduino.h>
#include <WebServer.h>
#include "Types.h"

namespace Services {

class AuthManager {
public:
  void begin();
  void generateCsrfToken();
  String getCsrfToken() const;
  bool checkCsrfToken(WebServer& server) const;

  // JWT login: returns access token (15min) + refresh token (7day, one-time use)
  // R10B-5: now also generates + stores refresh token in NVS.
  bool login(const String& user, const String& pass,
             String& outAccessToken, String& outRefreshToken,
             String& outCsrf, uint32_t& outAccessExp);

  // R10B-5: Refresh access token using refresh token.
  // Validates old refresh token, rotates it (old invalidated, new issued).
  // Returns true on success, fills out* params.
  // Returns false if refresh token invalid/expired/reused → user must re-login.
  bool refreshTokens(const String& refreshToken,
                     String& outAccessToken, String& outRefreshToken,
                     String& outCsrf, uint32_t& outAccessExp);

  // Verify JWT access token from Authorization header or Cookie
  bool checkAuth(WebServer& server);
  // Logout: invalidate current session (clears cookies client-side + revokes refresh token)
  // R10B-5: now also removes refresh token from NVS if present in cookie
  void logout(WebServer& server);

  // Rate limiting
  bool isRateLimited(uint32_t ip) const;
  void recordAuthFailure(uint32_t ip);
  void recordAuthSuccess(uint32_t ip);

  // Change password (verifies current, sets new with new salt)
  bool changePassword(const String& current, const String& next);

  // Factory reset token (two-step)
  String generateFactoryResetToken();
  bool consumeFactoryResetToken(const String& token);

private:
  bool _verifyPassword(const String& pass) const;
  // R10B-5: Refresh token NVS helpers
  bool _storeRefreshToken(const String& token);
  bool _isRefreshTokenValid(const String& token);
  void _invalidateRefreshToken(const String& token);
  String _generateRefreshToken();
};

extern AuthManager auth;

} // namespace Services

#endif
