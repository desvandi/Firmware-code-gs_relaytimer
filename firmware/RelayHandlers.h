// =============================================================================
// Web/Handlers/RelayHandlers.h — /api/relay
// =============================================================================
// PD-001 (Phase 6): REST ingress now uses the SHARED CommandCanonicalizer +
//   TransactionJournal path (same as MQTT). This handler:
//     1. Implies (type="relay", action=<action>) from the REST endpoint.
//     2. Validates transactionId (requestId) via the shared canonicalizer.
//     3. Computes the SAME commandHash that MQTT would compute for the same
//        logical command (cross-transport hash equivalence, AC-001/AC-018/AC-007).
//     4. Checks the journal for duplicates/conflicts (AC-009/AC-010/AC-011).
//     5. On NEW: executes, stores {requestId, commandHash, ackJson} in journal.
//
// Regression notes:
//   - The wire-level JSON body is unchanged: { channelId, action, mode?, manualState?, requestId }
//   - The success response shape is unchanged: { success, message, data: { channel: {...} } }
//   - The added fields (requestId, commandHash) are included in the response
//     data so PWA can correlate, but PWA does NOT depend on them.
//   - Per directive §21: PWA already sends requestId for REST mutations
//     (P2-2 reconciliation, see src/lib/api.ts).
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_RELAY_H
#define TIMER12_WEB_HANDLERS_RELAY_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RelayEngine.h"
#include "AuthManager.h"
#include "Config.h"
#include "Globals.h"

namespace Web { namespace Handlers {

// POST /api/relay { channelId, action, mode?, manualState?, requestId }
// audit-fixes-v2 (auditor #4 P1-1): REST API previously accepted `action=toggle`
//   which is non-idempotent. MQTT command contract already removed toggle for
//   idempotency (only on/off/set_mode). REST API now matches — toggle is
//   rejected with 400. Idempotent mutations are critical for retry safety:
//   request → timeout → retry must not flip state twice.
inline void handleRelay() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
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

  // --- Field validation (pre-canonical) ---
  int channelId = doc["channelId"] | 0;
  const char* actionStr = doc["action"] | "";
  if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
    sendError(400, "Invalid channelId (1-12)");
    return;
  }
  uint8_t idx = channelId - 1;

  // Validate action BEFORE execution (existing behavior — preserved).
  // Per directive §14: known/registered combination only.
  bool actionValid = (strcmp(actionStr, "on") == 0 || strcmp(actionStr, "off") == 0 ||
                      strcmp(actionStr, "set_mode") == 0);
  if (!actionValid) {
    sendError(400, "Invalid action (use on/off/set_mode — toggle removed for idempotency)");
    return;
  }

  // For set_mode, validate mode BEFORE transaction (existing behavior — preserved).
  if (strcmp(actionStr, "set_mode") == 0) {
    const char* modeStr = doc["mode"] | "";
    if (strcmp(modeStr, "auto") != 0 && strcmp(modeStr, "manual") != 0) {
      sendError(400, "Invalid mode");
      return;
    }
  }

  // --- PD-001: Canonical command model integration ---
  // REST endpoint implies (type="relay", action=<action>). Inject these into
  // the doc so the shared canonicalizer can hash the same way MQTT does.
  doc["type"] = "relay";
  doc["action"] = actionStr;  // already validated above

  RestTransaction tx = beginTransaction(doc);
  if (!tx.ok) {
    sendError(400, tx.errorMessage);
    return;
  }
  if (tx.decision == Services::TransactionDecision::CONFLICT) {
    rejectConflict(tx);
    return;
  }
  if (tx.decision == Services::TransactionDecision::DUPLICATE) {
    replayDuplicate(tx);
    return;
  }

  // --- NEW: Execute the mutation ---
  if (strcmp(actionStr, "on") == 0) {
    Services::relayEngine.setManual(idx, true);
  } else if (strcmp(actionStr, "off") == 0) {
    Services::relayEngine.setManual(idx, false);
  } else if (strcmp(actionStr, "set_mode") == 0) {
    const char* modeStr = doc["mode"] | "";
    bool manualState = doc["manualState"] | false;
    if (strcmp(modeStr, "auto") == 0) {
      Services::relayEngine.setMode(idx, true);
    } else if (strcmp(modeStr, "manual") == 0) {
      Services::relayEngine.setMode(idx, false);
      if (manualState) Services::relayEngine.setManual(idx, true);
      else Services::relayEngine.setManual(idx, false);
    }
  }

  // --- Build success ACK JSON ---
  // Shape preserved from original. Added requestId + commandHash to data
  // for PWA correlation (optional — PWA does not depend on these fields).
  String data = "{\"channel\":{\"id\":";
  data += String(channelId);
  data += ",\"name\":\"";
  data += Core::channels[idx].name;
  data += "\",\"modeAuto\":";
  data += Core::channels[idx].modeAuto ? "true" : "false";
  data += ",\"manualState\":";
  data += Core::channels[idx].manualState ? "true" : "false";
  data += ",\"state\":";
  data += Core::relayState[idx] ? "true" : "false";
  const char* srcStr =
    Core::relaySource[idx] == Core::RelaySource::Manual ? "manual" :
    Core::relaySource[idx] == Core::RelaySource::Schedule ? "schedule" :
    Core::relaySource[idx] == Core::RelaySource::Pir ? "pir" : "off";
  data += ",\"source\":\"";
  data += srcStr;
  data += "\"},\"requestId\":\"";
  data += tx.transactionId;
  data += "\",\"commandHash\":\"";
  data += tx.commandHash;
  data += "\"}";

  // --- Build full ACK envelope (must match sendSuccess shape) ---
  String ackJson = "{\"success\":true,\"message\":\"Relay updated\",\"data\":";
  ackJson += data;
  ackJson += "}";

  // --- Commit to journal (durable layer owned by TransactionJournal/PD-002) ---
  // Per directive §17: committing to RAM is NOT a durability claim. The
  // TransactionJournal's NVS-backed two-phase commit provides durability;
  // PD-001 only wires REST into that existing path.
  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  // --- Send response (same bytes as stored in journal) ---
  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

}} // namespace Web::Handlers

#endif
