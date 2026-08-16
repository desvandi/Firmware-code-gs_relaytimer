// =============================================================================
// Web/Handlers/SystemHandlers.h — /api/reboot
// =============================================================================
// PD-001 (Phase 6): REST ingress now uses the SHARED CommandCanonicalizer +
//   TransactionJournal path. Canonical mapping:
//     POST /api/reboot  →  type="system", action="reboot"
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_SYSTEM_H
#define TIMER12_WEB_HANDLERS_SYSTEM_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "ConfigStore.h"
#include "AuthManager.h"
#include "LogService.h"
#include "Config.h"

namespace Web { namespace Handlers {

// POST /api/reboot { requestId? }
// Note: reboot is a one-way operation — after ESP.restart() the device
// reboots and the HTTP response is best-effort. We still journal it so
// that a duplicate reboot request after partial network failure is
// detected (idempotent replay — the device is already rebooting).
inline void handleReboot() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;

  // Parse body if present (requestId is optional for backward compat).
  // Per directive §20: existing protocol must be preserved unless it
  // conflicts with the locked contract. /api/reboot historically had no
  // body — we accept an empty body or a JSON body with requestId.
  DynamicJsonDocument doc(256);
  if (Web::http.hasArg("plain") && Web::http.arg("plain").length() > 0) {
    DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
    if (err) {
      sendError(400, "Invalid JSON");
      return;
    }
  }

  // --- PD-001: Canonical command model integration ---
  doc["type"] = "system";
  doc["action"] = "reboot";

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

  // --- Build success ACK JSON (sent BEFORE reboot) ---
  String data = "{\"rebooting\":true,\"requestId\":\"";
  data += tx.transactionId;
  data += "\",\"commandHash\":\"";
  data += tx.commandHash;
  data += "\"}";

  String ackJson = "{\"success\":true,\"message\":\"System rebooting\",\"data\":";
  ackJson += data;
  ackJson += "}";

  // --- Commit to journal BEFORE sending response / rebooting ---
  // This ensures the transaction is durable even if reboot interrupts
  // the HTTP response. A duplicate reboot request after restart will be
  // detected as DUPLICATE and replayed.
  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  // --- Send response ---
  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);

  // --- Execute reboot (after response is sent) ---
  if (Core::scheduleDirty) Storage::config.saveSchedule(true);
  Services::Log.append(Core::LogType::Restart, "Reboot triggered via REST", 0);
  delay(500);
  ESP.restart();
}

}} // namespace Web::Handlers

#endif
