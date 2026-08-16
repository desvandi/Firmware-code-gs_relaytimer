// =============================================================================
// Web/Handlers/PirHandlers.h — /api/pir, /api/pir/test
// =============================================================================
// PD-001 (Phase 6): REST ingress now uses the SHARED CommandCanonicalizer +
//   TransactionJournal path. Canonical mappings:
//     POST /api/pir       →  type="pir", action="config"
//     POST /api/pir/test  →  type="pir", action="test"
//   Cross-transport hash equivalence: a PIR command via REST produces the
//   SAME commandHash as the equivalent MQTT command (AC-001/AC-018).
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_PIR_H
#define TIMER12_WEB_HANDLERS_PIR_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "PirDriver.h"
#include "AuthManager.h"
#include "ConfigStore.h"
#include "Config.h"
#include "Globals.h"

namespace Web { namespace Handlers {

// POST /api/pir { id, enabled?, holdTime?, requestId }
inline void handlePirConfig() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(256)) return;
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
  int id = doc["id"] | 0;
  if (id < 1 || id > (int)Core::NUM_PIR) {
    sendError(400, "Invalid PIR id (1-4)");
    return;
  }
  uint8_t idx = id - 1;
  uint8_t chIdx = Core::PIR_CHANNEL_OFFSET + idx;

  // --- PD-001: Canonical command model integration ---
  doc["type"] = "pir";
  doc["action"] = "config";

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

  // --- NEW: Execute ---
  if (doc.containsKey("enabled")) {
    Core::channels[chIdx].pirEnabled = doc["enabled"].as<bool>();
  }
  if (doc.containsKey("holdTime")) {
    int ht = doc["holdTime"] | 120;
    if (ht < 5) ht = 5;
    if (ht > 600) ht = 600;
    Core::channels[chIdx].pirHoldTime = (uint16_t)ht;
  }
  Storage::config.markDirty();

  // --- Build success ACK JSON ---
  String data = "{\"pir\":{\"id\":";
  data += String(id);
  data += ",\"channelId\":";
  data += String(chIdx + 1);
  data += ",\"enabled\":";
  data += Core::channels[chIdx].pirEnabled ? "true" : "false";
  data += ",\"holdTime\":";
  data += String(Core::channels[chIdx].pirHoldTime);
  data += "},\"requestId\":\"";
  data += tx.transactionId;
  data += "\",\"commandHash\":\"";
  data += tx.commandHash;
  data += "\"}";

  String ackJson = "{\"success\":true,\"message\":\"PIR config updated\",\"data\":";
  ackJson += data;
  ackJson += "}";

  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

// POST /api/pir/test { id, requestId? }
// Note: PIR test triggers a momentary physical relay pulse — it IS a mutation
// and IS routed through the journal (matches MQTT behavior where type=pir
// action=test is also journaled).
inline void handlePirTest() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(64)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  DynamicJsonDocument doc(64);
  DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
  if (err) {
    sendError(400, "Invalid JSON");
    return;
  }
  int id = doc["id"] | 0;
  if (id < 1 || id > (int)Core::NUM_PIR) {
    sendError(400, "Invalid PIR id (1-4)");
    return;
  }
  if (millis() < Core::pirStartupTime + Core::PIR_WARMUP_MS) {
    sendError(400, "PIR in warm-up");
    return;
  }

  // --- PD-001: Canonical command model integration ---
  doc["type"] = "pir";
  doc["action"] = "test";

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

  // --- NEW: Execute ---
  Drivers::pir.testTrigger(id - 1);

  // --- Build success ACK JSON ---
  String data = "{\"triggered\":true,\"requestId\":\"";
  data += tx.transactionId;
  data += "\",\"commandHash\":\"";
  data += tx.commandHash;
  data += "\"}";

  String ackJson = "{\"success\":true,\"message\":\"PIR triggered\",\"data\":";
  ackJson += data;
  ackJson += "}";

  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

}} // namespace Web::Handlers

#endif
