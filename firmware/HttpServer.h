// =============================================================================
// Web/HttpServer.h — Main HTTP server (v4.0 contract)
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_SERVER_H
#define TIMER12_WEB_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include "Config.h"

namespace Web {

class HttpServer {
public:
  void begin();
  void handleClient();

private:
  void _registerRoutes();
  // Helper: send security headers
  void _sendSecurityHeaders();
  // Helper: send JSON response envelope
  void _sendJson(const String& body, int code = 200);
  void _sendJsonSuccess(const String& message = "", const String& dataJson = "{}");
  void _sendJsonError(int code, const String& message);
};

extern HttpServer server;
extern WebServer http;  // underlying ESP32 WebServer instance

} // namespace Web

#endif
