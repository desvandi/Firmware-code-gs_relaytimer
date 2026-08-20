// =============================================================================
// Web/HttpServer.cpp — Main HTTP server setup + dispatch
// =============================================================================
#include "HttpServer.h"
#include "AuthHandlers.h"
#include "StatusHandlers.h"
#include "InsightsHandlers.h"  // Phase B (P0-01): /api/insights endpoint
#include "Ed25519SelfTest.h"   // On-target Ed25519 KAT
#include "RelayHandlers.h"
#include "ScheduleHandlers.h"
#include "ChannelHandlers.h"  // audit-fixes-v2 (P1-1): /api/channel endpoint
#include "PirHandlers.h"
#include "TimeHandlers.h"
#include "LogHandlers.h"
#include "ConfigHandlers.h"
#include "SystemHandlers.h"
#include "OtaHandlers.h"
#include "FactoryResetHandlers.h"

namespace Web {

WebServer http(80);
HttpServer server;

void HttpServer::_sendSecurityHeaders() {
  http.sendHeader("X-Frame-Options", "DENY");
  http.sendHeader("X-Content-Type-Options", "nosniff");
  http.sendHeader("Cache-Control", "no-store");
  http.sendHeader("Referrer-Policy", "no-referrer");
  http.sendHeader("Content-Security-Policy",
    "default-src 'self'; script-src 'self' 'unsafe-inline'; "
    "style-src 'self' 'unsafe-inline'; img-src 'self' data:;");

  // P0 #10 (audit round 9): CORS origin now configurable.
  // Default: "*" (development). Production: set ALLOWED_CORS_ORIGINS in Config.h
  // to PWA's Vercel URL.
  String origin = "*";
  if (strcmp(Core::ALLOWED_CORS_ORIGINS, "*") != 0) {
    // Echo back Origin if it's in the allowed list
    if (http.hasHeader("Origin")) {
      String reqOrigin = http.header("Origin");
      String allowed = Core::ALLOWED_CORS_ORIGINS;
      int start = 0;
      while (start < (int)allowed.length()) {
        int comma = allowed.indexOf(',', start);
        String one = (comma < 0) ? allowed.substring(start) : allowed.substring(start, comma);
        one.trim();
        if (one.length() > 0 && reqOrigin == one) {
          origin = reqOrigin;
          break;
        }
        if (comma < 0) break;
        start = comma + 1;
      }
    }
    if (origin == "*") origin = "";  // don't send ACAO if origin not allowed
  }

  if (origin.length() > 0) {
    http.sendHeader("Access-Control-Allow-Origin", origin);
    http.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    http.sendHeader("Access-Control-Allow-Headers",
      "Content-Type, Authorization, X-CSRF-Token");
    http.sendHeader("Access-Control-Allow-Credentials", "true");
    if (origin != "*") {
      http.sendHeader("Vary", "Origin");
    }
  }
}

void HttpServer::_sendJson(const String& body, int code) {
  _sendSecurityHeaders();
  http.send(code, "application/json; charset=utf-8", body);
}

void HttpServer::_sendJsonSuccess(const String& message, const String& dataJson) {
  String body = "{\"success\":true,\"message\":\"";
  body += message;
  body += "\",\"data\":";
  body += dataJson;
  body += "}";
  _sendJson(body, 200);
}

void HttpServer::_sendJsonError(int code, const String& message) {
  String body = "{\"success\":false,\"message\":\"";
  body += message;
  body += "\",\"data\":null}";
  _sendJson(body, code);
}

void HttpServer::_registerRoutes() {
  // CORS preflight (P0 #10: configurable origin)
  http.onNotFound([]() {
    if (http.method() == HTTP_OPTIONS) {
      http.sendHeader("X-Frame-Options", "DENY");
      http.sendHeader("X-Content-Type-Options", "nosniff");
      // Configurable CORS
      String origin = "*";
      if (strcmp(Core::ALLOWED_CORS_ORIGINS, "*") != 0) {
        if (http.hasHeader("Origin")) {
          String reqOrigin = http.header("Origin");
          String allowed = Core::ALLOWED_CORS_ORIGINS;
          int start = 0;
          while (start < (int)allowed.length()) {
            int comma = allowed.indexOf(',', start);
            String one = (comma < 0) ? allowed.substring(start) : allowed.substring(start, comma);
            one.trim();
            if (one.length() > 0 && reqOrigin == one) { origin = reqOrigin; break; }
            if (comma < 0) break;
            start = comma + 1;
          }
        }
        if (origin == "*") origin = "";
      }
      if (origin.length() > 0) {
        http.sendHeader("Access-Control-Allow-Origin", origin);
        http.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        http.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-CSRF-Token");
        http.sendHeader("Access-Control-Allow-Credentials", "true");
        if (origin != "*") http.sendHeader("Vary", "Origin");
      }
      http.send(204);
    } else {
      http.sendHeader("X-Frame-Options", "DENY");
      String origin = (strcmp(Core::ALLOWED_CORS_ORIGINS, "*") == 0) ? "*" : "";
      if (origin.length() > 0) {
        http.sendHeader("Access-Control-Allow-Origin", origin);
        http.sendHeader("Access-Control-Allow-Credentials", "true");
      }
      http.send(404, "application/json; charset=utf-8",
               "{\"success\":false,\"message\":\"Not Found\",\"data\":null}");
    }
  });

  // Auth
  http.on("/api/login", HTTP_POST, Web::Handlers::handleLogin);
  http.on("/api/logout", HTTP_POST, Web::Handlers::handleLogout);
  http.on("/api/session", HTTP_GET, Web::Handlers::handleSession);
  http.on("/api/refresh", HTTP_POST, Web::Handlers::handleRefresh);  // R10B-5

  // Status
  http.on("/api/status", HTTP_GET, Web::Handlers::handleStatus);
  http.on("/api/version", HTTP_GET, Web::Handlers::handleVersion);
  http.on("/api/health", HTTP_GET, Web::Handlers::handleHealth);

  // Phase B (P0-01): AI insights — ESP32 proxies authenticated GET to GAS.
  http.on("/api/insights", HTTP_GET, Web::Handlers::handleInsights);

  // On-target Ed25519 KAT self-test (authenticated, 5-min cache)
  http.on("/api/ed25519/kat", HTTP_GET, []() {
    if (!requireAuth()) return;
    bool force = Web::http.hasArg("force") && Web::http.arg("force") == "1";
    Services::Ed25519KatResult r = Services::ed25519SelfTest.run(force);
    String data = "{";
    data += "\"allPassed\":" + String(r.allPassed ? "true" : "false") + ",";
    data += "\"testsRun\":" + String(r.testsRun) + ",";
    data += "\"testsPassed\":" + String(r.testsPassed) + ",";
    data += "\"testsFailed\":" + String(r.testsFailed) + ",";
    data += "\"totalDurationMs\":" + String(r.totalDurationMs) + ",";
    data += "\"libraryVersion\":\"" + String(r.libraryVersion) + "\",";
    data += "\"cached\":" + String(r.cached ? "true" : "false");
    data += "}";
    sendSuccess("Ed25519 KAT result", data);
  });

  // Relay
  http.on("/api/relay", HTTP_POST, Web::Handlers::handleRelay);

  // Channel rename (audit-fixes-v2 P1-1: was missing — PWA called /api/channel
  //   in LAN REST mode but firmware returned 404. MQTT mode worked via
  //   type="channel" command.)
  http.on("/api/channel", HTTP_POST, Web::Handlers::handleChannelRename);

  // Schedule
  http.on("/api/schedule", HTTP_POST, Web::Handlers::handleScheduleUpsert);
  http.on("/api/schedule", HTTP_DELETE, Web::Handlers::handleScheduleDelete);

  // PIR
  http.on("/api/pir", HTTP_POST, Web::Handlers::handlePirConfig);
  http.on("/api/pir/test", HTTP_POST, Web::Handlers::handlePirTest);

  // Time
  http.on("/api/time", HTTP_POST, Web::Handlers::handleSetTime);

  // Logs
  http.on("/api/log", HTTP_GET, Web::Handlers::handleGetLogs);
  http.on("/api/audit_log", HTTP_GET, Web::Handlers::handleGetAuditLog);

  // Config
  http.on("/api/config", HTTP_GET, Web::Handlers::handleGetConfig);
  http.on("/api/config", HTTP_POST, Web::Handlers::handleSetConfig);
  http.on("/api/config/device", HTTP_POST, Web::Handlers::handleSetDeviceConfig);
  http.on("/api/config/password", HTTP_POST, Web::Handlers::handleChangePassword);
  http.on("/api/config/export", HTTP_GET, Web::Handlers::handleExportConfig);
  http.on("/api/config/import", HTTP_POST, Web::Handlers::handleImportConfig);

  // System
  http.on("/api/reboot", HTTP_POST, Web::Handlers::handleReboot);

  // OTA
  http.on("/api/ota", HTTP_POST, Web::Handlers::handleOtaResponse, Web::Handlers::handleOtaUpload);
  http.on("/api/ota/check", HTTP_POST, Web::Handlers::handleOtaCheck);

  // Factory reset (two-step)
  http.on("/api/factory_reset/prepare", HTTP_POST, Web::Handlers::handleFactoryResetPrepare);
  http.on("/api/factory_reset/confirm", HTTP_POST, Web::Handlers::handleFactoryResetConfirm);

  // Collect headers we need
  // REAUDIT-FW-OTA-002: Added X-Expected-SHA256 + X-Firmware-Version for REST OTA
  const char* headerKeys[] = {"Authorization", "X-CSRF-Token", "Content-Length", "Cookie", "Origin",
                              "X-Expected-SHA256", "X-Firmware-Version"};
  http.collectHeaders(headerKeys, 7);
}

void HttpServer::begin() {
  _registerRoutes();
  http.begin();
}

void HttpServer::handleClient() {
  http.handleClient();
}

} // namespace Web
