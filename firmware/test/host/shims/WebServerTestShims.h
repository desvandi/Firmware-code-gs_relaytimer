// =============================================================================
// WebServerTestShims.h — Shims for WebServerTest (F-P0-2 C2)
// =============================================================================
// Provides a RICHER WebServer mock than MqttClientDeps.h's minimal stub.
// The WebServer test needs to:
//   - Set request body (via arg("plain"))
//   - Set request headers (Authorization, X-CSRF-Token, Content-Length, Origin)
//   - Capture response (status code + body)
//   - Reset state between tests
//
// This shim is FORCE-INCLUDED via -include in Makefile.ws, BEFORE any
// firmware headers. It overrides MqttClientDeps.h's minimal WebServer
// by redefining the class with richer functionality.
//
// Strategy: define TIMER12_WEB_SERVER_H guard to skip firmware/WebServer.h,
// then provide our own WebServer class with state capture.
// =============================================================================
#pragma once
#ifndef TIMER12_TEST_WEBSERVER_SHIMS_H
#define TIMER12_TEST_WEBSERVER_SHIMS_H

// We will be force-included BEFORE MqttClientDeps.h. But MqttClientDeps.h
// defines its own WebServer. To override, we use a different approach:
// include MqttClientDeps.h first, then #undef and redefine.
//
// Actually, simpler: Makefile.ws will include this file via -include AFTER
// MqttClientDeps.h. So MqttClientDeps.h's WebServer is already defined.
// We can't redefine it. Instead, we extend it via free functions that
// access a global test-state struct.
//
// Better approach: Makefile.ws will NOT include MqttClientDeps.h. Instead,
// this file provides EVERYTHING needed (a superset of MqttClientDeps.h's
// WebServer + the test extensions).

// For now, let's use a simpler approach: this file is included AFTER
// MqttClientDeps.h (via -include order), and it provides additional
// global state + helper functions that the test uses. The WebServer
// class itself comes from MqttClientDeps.h, but we add a global
// "test request context" that the test sets before calling handleRelay().

#include <Arduino.h>  // for String
#include <cstring>
#include <cstdio>

// =============================================================================
// Test request/response context — set by test, read by handlers
// =============================================================================
namespace WebServerTest {

// Maximum sizes for test buffers
constexpr size_t MAX_BODY = 4096;
constexpr size_t MAX_HEADER = 256;
constexpr size_t MAX_RESPONSE = 8192;

// Request state (set by test before calling handler)
struct RequestState {
  char body[MAX_BODY];           // request body (the "plain" arg)
  size_t bodyLen;
  char authHeader[MAX_HEADER];  // Authorization header value
  char csrfHeader[MAX_HEADER];  // X-CSRF-Token header value
  char contentLength[16];        // Content-Length header value
  char origin[MAX_HEADER];       // Origin header value
  bool hasAuthHeader;
  bool hasCsrfHeader;
  bool hasContentLength;
  bool hasOrigin;
};

// Response state (captured by handler's send() calls)
struct ResponseState {
  int code;                      // HTTP status code
  char body[MAX_RESPONSE];       // response body
  size_t bodyLen;
  // Captured headers (simplified — just ACAO for now)
  char acaoHeader[MAX_HEADER];   // Access-Control-Allow-Origin
  bool hasAcao;
};

// Global state — test sets request, handler fills response
inline RequestState g_request;
inline ResponseState g_response;

// Reset state between tests
inline void resetRequest() {
  g_request.body[0] = '\0';
  g_request.bodyLen = 0;
  g_request.authHeader[0] = '\0';
  g_request.csrfHeader[0] = '\0';
  g_request.contentLength[0] = '\0';
  g_request.origin[0] = '\0';
  g_request.hasAuthHeader = false;
  g_request.hasCsrfHeader = false;
  g_request.hasContentLength = false;
  g_request.hasOrigin = false;
}

inline void resetResponse() {
  g_response.code = 0;
  g_response.body[0] = '\0';
  g_response.bodyLen = 0;
  g_response.acaoHeader[0] = '\0';
  g_response.hasAcao = false;
}

inline void resetAll() {
  resetRequest();
  resetResponse();
}

// Helper to set request body
inline void setRequestBody(const char* body) {
  size_t len = strlen(body);
  if (len >= MAX_BODY) len = MAX_BODY - 1;
  memcpy(g_request.body, body, len);
  g_request.body[len] = '\0';
  g_request.bodyLen = len;
  // Also set Content-Length header
  snprintf(g_request.contentLength, sizeof(g_request.contentLength), "%zu", len);
  g_request.hasContentLength = true;
}

// Helper to set auth header (simulates valid JWT)
inline void setAuthValid() {
  strncpy(g_request.authHeader, "Bearer valid-jwt-token", MAX_HEADER - 1);
  g_request.authHeader[MAX_HEADER - 1] = '\0';
  g_request.hasAuthHeader = true;
}

// Helper to set CSRF token (simulates valid token)
inline void setCsrfValid() {
  strncpy(g_request.csrfHeader, "valid-csrf-token", MAX_HEADER - 1);
  g_request.csrfHeader[MAX_HEADER - 1] = '\0';
  g_request.hasCsrfHeader = true;
}

} // namespace WebServerTest

#endif // TIMER12_TEST_WEBSERVER_SHIMS_H
