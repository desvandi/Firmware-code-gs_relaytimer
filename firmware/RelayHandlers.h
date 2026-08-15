// =============================================================================
// Web/Handlers/RelayHandlers.h — /api/relay
// =============================================================================
// P2-2 F-P0-2 C2: Refactored to use Web::Rest journal lifecycle helpers.
//
// AUDITOR SCOPE (Phase B REV.3 §4 + C2 directive):
//   This is the PROOF-OF-PATTERN handler — first REST endpoint to go through
//   the TransactionJournal. Pattern will be replicated to other handlers in
//   later C-phases (C3+).
//
// COMMIT MODE: EXECUTING
//   Relay is a physical GPIO mutation — externally observable, may not be
//   safely retryable if crash occurs mid-mutation. Uses markExecuting()
//   before GPIO write, commitTransaction() after success.
//
// FLOW (per auditor Phase B REV.3 §4.5):
//   1. requireAuth, requireCsrf, requireBody
//   2. parse JSON
//   3. domain validation (channelId, action)
//   4. [helper] validateRequestId
//   5. [helper] computeCommandHash
//   6. [helper] checkDuplicateAndRespond (replay ACK if duplicate)
//   7. [helper] storeIntentOrReject (PENDING)
//   8. [helper] markExecutingOrAbort (EXECUTING)
//   9. [handler] ACTUAL MUTATION (relayEngine.setManual/setMode)
//  10. [handler] build ACK JSON (domain-specific shape with requestId)
//  11. [helper] commitExecutingOrFailure (COMMITTED + ACK queued)
//  12. [handler] send HTTP 200 with ackJson
//
// HARD INVARIANT (Phase B REV.3 §9.4):
//   Web::http.send(200, ...) only after commitExecutingOrFailure returns true.
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_RELAY_H
#define TIMER12_WEB_HANDLERS_RELAY_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RestJournalHelper.h"  // P2-2 F-P0-2 C2: journal lifecycle helpers
#include "RelayEngine.h"
#include "AuthManager.h"
#include "Config.h"
#include "Globals.h"
#include "RtcDriver.h"  // for Drivers::rtc.getUnixTime() in ACK timestamp

namespace Web { namespace Handlers {

// POST /api/relay { channelId, action, mode?, manualState?, requestId }
// audit-fixes-v2 (auditor #4 P1-1): REST API previously accepted `action=toggle`
//   which is non-idempotent. MQTT command contract already removed toggle for
//   idempotency (only on/off/set_mode). REST API now matches — toggle is
//   rejected with 400. Idempotent mutations are critical for retry safety:
//   request → timeout → retry must not flip state twice.
//
// P2-2 F-P0-2 C2: REST now requires `requestId` field (mirrors MQTT contract).
//   Old PWA builds without requestId will receive HTTP 400. PWA must be updated
//   to generate UUID per logical mutation (see Phase B REV.3 §1.3.1).
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

  // ---- Domain validation (channelId, action) BEFORE journal ----
  // Validates fields that don't require journal state. If validation fails
  // here, NO journal entry is created (rule #1 of universal failure invariant).
  int channelId = doc["channelId"] | 0;
  const char* actionStr = doc["action"] | "";
  if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
    sendError(400, "Invalid channelId (1-12)");
    return;
  }
  uint8_t idx = channelId - 1;

  // Validate action is one of allowed values (do NOT execute yet)
  bool validAction = (strcmp(actionStr, "on") == 0 ||
                      strcmp(actionStr, "off") == 0 ||
                      strcmp(actionStr, "set_mode") == 0);
  if (!validAction) {
    sendError(400, "Invalid action (use on/off/set_mode — toggle removed for idempotency)");
    return;
  }

  // For set_mode, validate mode field BEFORE journal entry
  bool intentDesiredState = false;
  if (strcmp(actionStr, "on") == 0) {
    intentDesiredState = true;
  } else if (strcmp(actionStr, "off") == 0) {
    intentDesiredState = false;
  } else { // set_mode
    const char* modeStr = doc["mode"] | "";
    if (strcmp(modeStr, "auto") == 0) {
      intentDesiredState = Core::relayState[idx];  // mode change doesn't directly set state
    } else if (strcmp(modeStr, "manual") == 0) {
      intentDesiredState = doc["manualState"] | false;
    } else {
      sendError(400, "Invalid mode (use auto/manual)");
      return;
    }
  }

  // ---- [helper] requestId validation ----
  String requestId;
  if (!Web::Rest::validateRequestId(doc["requestId"] | "")) {
    // Helper already sent HTTP 400
    return;
  }
  requestId = String(doc["requestId"] | "");

  // ---- [helper] command hash (uses shared Utils::computeCommandHash) ----
  String commandHash = Web::Rest::computeCommandHash(doc);

  // ---- [helper] duplicate check + ACK replay ----
  // If requestId already in journal, this sends the appropriate response
  // (200 replay, 409 in-progress, 409 reuse-rejected) and returns true.
  if (Web::Rest::checkDuplicateAndRespond(requestId, commandHash)) {
    return;  // already responded — do NOT re-execute
  }

  // ---- [helper] storeIntent (PENDING) ----
  // Capture intent BEFORE mutation. If crash occurs after this, journal
  // shows PENDING — recovery can detect in-flight command.
  if (!Web::Rest::storeIntentOrReject(requestId, commandHash,
                                        (uint8_t)channelId,
                                        intentDesiredState,
                                        Core::relayState[idx])) {
    // Helper already sent HTTP 503 DURABILITY_FAILURE
    return;
  }

  // ---- [helper] markExecuting (EXECUTING) ----
  // Physical mutation is about to begin. If crash occurs after this,
  // journal shows EXECUTING — recovery knows mutation may have started.
  if (!Web::Rest::markExecutingOrAbort(requestId)) {
    // Helper already sent HTTP 503 + clearEntry (no mutation occurred)
    return;
  }

  // ---- [handler] ACTUAL MUTATION (relayEngine calls) ----
  // This is the physical GPIO write. After this point, INVARIANT B applies:
  // mutation may have occurred, so clearEntry is FORBIDDEN on failure.
  if (strcmp(actionStr, "on") == 0) {
    Services::relayEngine.setManual(idx, true);
  } else if (strcmp(actionStr, "off") == 0) {
    Services::relayEngine.setManual(idx, false);
  } else if (strcmp(actionStr, "set_mode") == 0) {
    const char* modeStr = doc["mode"] | "";
    bool manualState = doc["manualState"] | false;
    if (strcmp(modeStr, "auto") == 0) {
      Services::relayEngine.setMode(idx, true);
    } else { // manual (already validated above)
      Services::relayEngine.setMode(idx, false);
      Services::relayEngine.setManual(idx, manualState);
    }
  }

  // ---- [handler] build ACK JSON (domain-specific shape) ----
  // ACK includes: requestId, success, message, timestamp, data{channel{...}}
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
  data += "\"}}";

  // Build full ACK envelope (matches MQTT ACK shape for cross-ingress symmetry)
  // Note: timestamp is uint64_t (ms since epoch). Arduino String doesn't have
  // a uint64_t constructor, so we format it as a string first.
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%llu",
           (unsigned long long)Drivers::rtc.getUnixTime() * 1000ULL);
  String ackJson = "{\"requestId\":\"";
  ackJson += requestId;
  ackJson += "\",\"success\":true,\"message\":\"Relay updated\",\"timestamp\":";
  ackJson += tsBuf;
  ackJson += ",\"data\":";
  ackJson += data;
  ackJson += "}";

  // ---- [helper] commit (EXECUTING path) ----
  // Transitions EXECUTING → COMMITTED + queues ACK. If commit fails:
  //   - HTTP 503 sent (DURABILITY_FAILURE)
  //   - Journal state preserved as EXECUTING (INVARIANT B — evidence preserved)
  //   - clearEntry NOT called (mutation already occurred)
  if (!Web::Rest::commitExecutingOrFailure(requestId, ackJson)) {
    // Helper already sent HTTP 503 — DO NOT send HTTP 200
    return;
  }

  // ---- [handler] send HTTP 200 with ACK ----
  // HARD INVARIANT: HTTP 200 only sent AFTER commit succeeded.
  // Journal state == COMMITTED, ACK queued for retry if TCP fails.
  Web::sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

}} // namespace Web::Handlers

#endif
