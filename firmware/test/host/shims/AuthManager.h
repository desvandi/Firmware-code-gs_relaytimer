// =============================================================================
// Stub: AuthManager.h (overrides firmware/AuthManager.h via -I shims)
// =============================================================================
// MqttClient.cpp includes AuthManager.h but does NOT directly use the
// Services::auth singleton. The full firmware/AuthManager.h includes
// <WebServer.h> (which we stub) — our stub provides a class that compiles
// cleanly without referencing the real impl in AuthManager.cpp.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_AUTH_MANAGER_H
#define HOST_SHIM_AUTH_MANAGER_H

#include <Arduino.h>
#include <WebServer.h>
#include "Types.h"

namespace Services {

class AuthManager {
public:
  void begin() {}
  void generateCsrfToken() {}
  String getCsrfToken() const { return ""; }
  bool checkCsrfToken(WebServer&) const { return true; }
  bool login(const String&, const String&, String&, String&, String&, uint32_t&, uint32_t) {
    return false;
  }
  bool refreshTokens(const String&, String&, String&, String&, uint32_t&) {
    return false;
  }
  bool checkAuth(WebServer&) { return true; }
  void logout(WebServer&) {}
  bool isRateLimited(uint32_t) const { return false; }
  void recordAuthFailure(uint32_t) {}
  void recordAuthSuccess(uint32_t) {}
  bool changePassword(const String&, const String&) { return false; }
  String generateFactoryResetToken() { return ""; }
  bool consumeFactoryResetToken(const String&) { return false; }
};

extern AuthManager auth;

} // namespace Services

#endif // HOST_SHIM_AUTH_MANAGER_H
