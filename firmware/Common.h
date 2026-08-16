// =============================================================================
// Web/Handlers/Common.h — Shared helpers used by all handlers
// =============================================================================
// P0 #10 (audit round 9): CORS origin now configurable via Config::ALLOWED_CORS_ORIGINS.
// Default: "*" (any origin — development only).
// Production: set to PWA's Vercel URL, e.g., "https://remote-relay.vercel.app".
//
// PD-001 (Phase 6): Added REST transaction helpers (beginTransaction /
//   commitTransaction / replayDuplicate / rejectConflict) so REST handlers
//   share the SAME canonicalization + dedup path as MQTT. This closes the
//   REST/MQTT asymmetry where REST previously had no transaction identity,
//   no command hash, and no journal integration.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_COMMON_H
#define TIMER12_WEB_HANDLERS_COMMON_H

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "AuthManager.h"
#include "HttpServer.h"
#include "Config.h"
#include "CommandCanonicalizer.h"
#include "TransactionJournal.h"
#include "LogService.h"

namespace Web {

// Determine the correct Access-Control-Allow-Origin header value for the
// incoming request. If Config::ALLOWED_CORS_ORIGINS is "*", returns "*".
// Otherwise, checks if the request's Origin header matches one of the
// comma-separated allowed origins and returns it (echo-back pattern for
// credentialed CORS). Returns empty string if origin not allowed.
inline String getAllowedOrigin() {
  if (strcmp(Core::ALLOWED_CORS_ORIGINS, "*") == 0) {
    return "*";
  }
  // Check Origin header against allowed list
  if (Web::http.hasHeader("Origin")) {
    String origin = Web::http.header("Origin");
    // Parse comma-separated list
    String allowed = Core::ALLOWED_CORS_ORIGINS;
    int start = 0;
    while (start < (int)allowed.length()) {
      int comma = allowed.indexOf(',', start);
      String one = (comma < 0) ? allowed.substring(start) : allowed.substring(start, comma);
      one.trim();
      if (one.length() > 0 && origin == one) {
        return origin;  // echo back the specific origin
      }
      if (comma < 0) break;
      start = comma + 1;
    }
  }
  return "";  // origin not allowed
}

// Send CORS + security headers (used by all JSON responses)
inline void sendSecurityHeaders() {
  String origin = getAllowedOrigin();
  if (origin.length() > 0) {
    Web::http.sendHeader("Access-Control-Allow-Origin", origin);
    Web::http.sendHeader("Access-Control-Allow-Credentials", "true");
    Web::http.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    Web::http.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-CSRF-Token");
    if (origin != "*") {
      Web::http.sendHeader("Vary", "Origin");
    }
  }
  Web::http.sendHeader("X-Frame-Options", "DENY");
  Web::http.sendHeader("X-Content-Type-Options", "nosniff");
  Web::http.sendHeader("Cache-Control", "no-store");
  Web::http.sendHeader("Referrer-Policy", "no-referrer");
}

// Send success envelope: { success: true, message, data }
inline void sendSuccess(const String& message, const String& dataJson = "{}") {
  String body = "{\"success\":true,\"message\":\"";
  body += message;
  body += "\",\"data\":";
  body += dataJson;
  body += "}";
  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", body);
}

// Send error envelope: { success: false, message, data: null }
inline void sendError(int code, const String& message) {
  String body = "{\"success\":false,\"message\":\"";
  body += message;
  body += "\",\"data\":null}";
  sendSecurityHeaders();
  Web::http.send(code, "application/json; charset=utf-8", body);
}

inline bool requireAuth() {
  return Services::auth.checkAuth(Web::http);
}

inline bool requireCsrf() {
  if (!Services::auth.checkCsrfToken(Web::http)) {
    sendError(403, "Invalid CSRF token");
    return false;
  }
  return true;
}

inline bool requireBody(size_t maxSize) {
  if (Web::http.hasHeader("Content-Length")) {
    size_t len = (size_t)Web::http.header("Content-Length").toInt();
    if (len > maxSize) {
      sendError(413, "Body too large");
      return false;
    }
  }
  return true;
}

// ===========================================================================
// PD-001 (Phase 6): REST transaction helpers.
//
// These helpers wire REST ingress into the SAME canonical command model used
// by MQTT. Each REST mutation handler should:
//
//   1. Parse JSON body into a JsonDocument.
//   2. Mutate the doc to add the canonical (type, action) pair corresponding
//      to the REST endpoint (REST endpoints do NOT carry "type"/"action" in
//      their JSON body — they're implied by the URL + method).
//   3. Call beginTransaction(doc) — this validates transactionId, computes
//      the canonical hash, and checks the journal.
//   4. If decision == CONFLICT  → call rejectConflict() and return.
//   5. If decision == DUPLICATE → call replayDuplicate() and return.
//   6. If decision == NEW       → execute the mutation, then call
//      commitTransaction() with the success ACK JSON.
//
// Per directive §16 / AC-009 / AC-010 / AC-011:
//   - DUPLICATE: replay original ACK, NO re-execution, NO journal overwrite.
//   - CONFLICT:  REJECT, NO mutation, NO journal overwrite.
//   - NEW:       execute, then store {requestId, commandHash, ackJson} in
//                journal (durable layer owned by TransactionJournal/PD-002).
// ===========================================================================

struct RestTransaction {
  bool ok;                                  // false if validation/canonicalization failed
  String transactionId;                     // validated transactionId
  String commandHash;                       // canonical SHA-256 hex
  Services::TransactionDecision decision;   // NEW / DUPLICATE / CONFLICT
  String previousAckJson;                   // populated if DUPLICATE
  String errorMessage;                      // populated if !ok
};

// Begin a REST transaction: validate + canonicalize + hash + journal lookup.
// This does NOT execute any mutation and does NOT modify the journal.
//
// doc IN/OUT: the parsed JSON body. The caller should set doc["type"] and
//   doc["action"] BEFORE calling this helper (REST endpoints imply a fixed
//   type/action pair that is not present in the wire JSON).
//
// TRANSACTION_ID_REQUIRED:
//   If true (default), transactionId MUST be present and valid — missing/
//   invalid → ok=false with errorMessage. This is the strict mode for
//   endpoints where the PWA already sends requestId (relay, channel,
//   schedule, pir).
//
//   If false, transactionId is OPTIONAL — if absent, the transaction is
//   allowed to proceed WITHOUT journal integration (decision=NEW, but
//   no hash computed, no journal lookup, no commit). This preserves
//   backward compatibility for endpoints where the PWA does not yet send
//   requestId (time, reboot, config/device). When the PWA is updated to
//   send requestId for these endpoints (PD-007), they will automatically
//   gain journal integration without further firmware changes.
inline RestTransaction beginTransaction(JsonDocument& doc,
                                        bool transactionIdRequired = true) {
  RestTransaction r;
  r.ok = false;
  r.decision = Services::TransactionDecision::NEW;

  // Extract transactionId (requestId compatibility).
  String tid = doc["requestId"] | "";
  String tidAlt = doc["transactionId"] | "";
  if (tid.length() == 0 && tidAlt.length() > 0) {
    tid = tidAlt;
  } else if (tid.length() > 0 && tidAlt.length() > 0 && tid != tidAlt) {
    r.errorMessage = "requestId and transactionId both present but differ";
    return r;
  }

  // Backward-compat path: transactionId not required and not provided.
  // Skip canonicalization + journal integration entirely. The command
  // proceeds as a non-journaled mutation (pre-PD-001 behavior).
  if (!transactionIdRequired && tid.length() == 0) {
    r.ok = true;
    r.transactionId = "";
    r.commandHash = "";
    r.decision = Services::TransactionDecision::NEW;
    r.previousAckJson = "";
    r.errorMessage = "";
    return r;
  }

  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) {
    r.errorMessage = canon.errorMessage;
    return r;
  }
  r.transactionId = canon.transactionId;
  r.commandHash = canon.commandHash;

  Services::DecisionResult d =
      Services::CommandCanonicalizer::decideTransaction(r.transactionId, r.commandHash);
  r.decision = d.decision;
  r.previousAckJson = d.previousAckJson;
  r.ok = true;
  return r;
}

// Commit a REST transaction AFTER successful mutation.
// Stores {requestId, commandHash, ackJson} in the journal.
//
// ackJson: the success ACK JSON that would be sent to the client. This is
//   stored so that a future DUPLICATE request can replay the EXACT same ACK.
//
// Returns true if stored successfully. If storage fails (NVS write error),
// the mutation has already happened — caller should still send the success
// response. The journal will miss this entry, so a retry may re-execute
// (safe for idempotent commands per existing durability documentation).
//
// If transactionId or commandHash is empty (backward-compat path where
// client did not send requestId), this is a NO-OP — the mutation proceeds
// without journal integration. This preserves pre-PD-001 behavior for
// endpoints where the PWA does not yet send requestId.
inline bool commitTransaction(const String& transactionId,
                              const String& commandHash,
                              const String& ackJson) {
  if (transactionId.length() == 0 || commandHash.length() == 0) {
    // Backward-compat: no transactionId → no journal integration.
    return false;
  }
  return Services::journal.storeTransaction(transactionId, commandHash, ackJson);
}

// Send a 409 Conflict response for a CONFLICT decision.
// Per directive §16 / AC-010 / AC-011: NO mutation, NO journal overwrite.
inline void rejectConflict(const RestTransaction& tx) {
  Services::Log.append(Core::LogType::AuthFail,
    "SECURITY: requestId reuse with different command (REST): " + tx.transactionId, 0);
  sendError(409, "requestId reuse with different command — rejected");
}

// Replay the original ACK for a DUPLICATE decision.
// Per directive §16 / AC-009: NO re-execution, NO journal overwrite.
inline void replayDuplicate(const RestTransaction& tx) {
  Serial.printf("[REST] Duplicate command detected: %s — replaying original ACK\n",
                tx.transactionId.c_str());
  if (tx.previousAckJson.length() > 0) {
    // Replay the EXACT original ACK bytes (including its own success/data fields).
    sendSecurityHeaders();
    Web::http.send(200, "application/json; charset=utf-8", tx.previousAckJson);
  } else {
    // Journal entry exists but ACK JSON is empty — shouldn't happen.
    sendSuccess("Duplicate command (already executed)");
  }
}

} // namespace Web

#endif
