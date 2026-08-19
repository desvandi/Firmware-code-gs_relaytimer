// =============================================================================
// Web/Handlers/TimeHandlers.h — /api/time
// =============================================================================
// PD-001 (Phase 6): REST ingress now uses the SHARED CommandCanonicalizer +
//   TransactionJournal path. Canonical mapping:
//     POST /api/time  →  type="time", action="set"
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_TIME_H
#define TIMER12_WEB_HANDLERS_TIME_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RtcDriver.h"
#include "AuthManager.h"
#include "Json.h"
#include "Config.h"

namespace Web { namespace Handlers {

// POST /api/time { datetime: "YYYY-MM-DDTHH:MM:SS", requestId }
inline void handleSetTime() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(128)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
  if (err) {
    sendError(400, "Invalid JSON");
    return;
  }
  const char* dt = doc["datetime"] | "";
  int y, m, d, h, mi, s;
  if (sscanf(dt, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &mi, &s) != 6) {
    sendError(400, "Invalid datetime (use ISO 8601: YYYY-MM-DDTHH:MM:SS)");
    return;
  }
  if (!Utils::isValidDate(y, m, d) || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
    sendError(400, "Invalid date/time components");
    return;
  }

  // --- PD-001: Canonical command model integration ---
  // transactionId is OPTIONAL for /api/time (PWA does not yet send requestId
  // for this endpoint as of P2-2 reconciliation). When absent, the command
  // proceeds without journal integration (pre-PD-001 behavior). When present
  // (future PWA update / PD-007), full journal integration applies.
  doc["type"] = "time";
  doc["action"] = "set";

  RestTransaction tx = beginTransaction(doc, /*transactionIdRequired=*/false);
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

  // --- NEW: Execute ---
  Drivers::rtc.adjust(y, m, d, h, mi, s);

  // --- Build success ACK JSON ---
  // Include requestId/commandHash only when present (backward compat with
  // PWA builds that do not send requestId for /api/time).
  String data = "{\"synced\":true";
  if (tx.transactionId.length() > 0) {
    data += ",\"requestId\":\"";
    data += tx.transactionId;
    data += "\",\"commandHash\":\"";
    data += tx.commandHash;
    data += "\"";
  }
  data += "}";

  String ackJson = "{\"success\":true,\"message\":\"RTC time synced\",\"data\":";
  ackJson += data;
  ackJson += "}";

  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

}} // namespace Web::Handlers

#endif
