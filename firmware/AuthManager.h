// =============================================================================
// Services/AuthManager.h — JWT + CSRF + rate limiting
// =============================================================================
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

  // JWT login: returns token string + expiry epoch
  bool login(const String& user, const String& pass, String& outToken,
             String& outCsrf, uint32_t& outExp);
  // Verify JWT from Authorization header or Cookie
  bool checkAuth(WebServer& server);
  // Logout: invalidate current session (cookie cleared client-side)
  void logout();

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
};

extern AuthManager auth;

} // namespace Services

#endif
