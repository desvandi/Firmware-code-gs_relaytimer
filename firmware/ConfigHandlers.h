// =============================================================================
// Web/Handlers/ConfigHandlers.h — /api/config, /api/config/*
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_CONFIG_H
#define TIMER12_WEB_HANDLERS_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "ConfigStore.h"
#include "AuthManager.h"
#include "LogService.h"
#include "Json.h"
#include "Crypto.h"
#include "Config.h"
#include "Globals.h"

namespace Web { namespace Handlers {

// GET /api/config → user info (no secrets)
inline void handleGetConfig() {
  if (!requireAuth()) return;
  String data = "{\"user\":\"";
  data += Core::userConfig.wwwUser;
  data += "\",\"iterations\":";
  data += String(Core::userConfig.iterations);
  data += ",\"deviceName\":\"";
  data += Core::deviceName;
  data += "\",\"timezone\":\"";
  data += Core::timezone;
  data += "\"}";
  sendSuccess("", data);
}

// POST /api/config { user?, pass? }  — legacy v3.1 compatibility
inline void handleSetConfig() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, Web::http.arg("plain"));
  if (err) {
    sendError(400, "Invalid JSON");
    return;
  }
  if (doc.containsKey("user")) {
    const char* u = doc["user"];
    if (u) {
      String newUser = u;
      newUser.trim();
      if (newUser.length() < 1 || newUser.length() > Core::MAX_USER_LEN) {
        sendError(400, "Username must be 1-31 chars");
        return;
      }
      strncpy(Core::userConfig.wwwUser, newUser.c_str(), Core::MAX_USER_LEN);
      Core::userConfig.wwwUser[Core::MAX_USER_LEN] = '\0';
    }
  }
  if (doc.containsKey("pass")) {
    const char* p = doc["pass"];
    if (p) {
      String newPass = p;
      if (!Utils::isPasswordStrong(newPass)) {
        sendError(400, "Password must be 8+ chars with letter+digit");
        return;
      }
      Utils::generateRandomBytes(Core::userConfig.salt, Core::SALT_LEN);
      Core::userConfig.iterations = Core::PBKDF2_ITERATIONS;
      uint8_t hash[32];
      if (!Utils::pbkdf2HmacSha256(newPass.c_str(), newPass.length(),
                                   Core::userConfig.salt, Core::SALT_LEN,
                                   Core::userConfig.iterations, hash)) {
        sendError(500, "Hash failed");
        return;
      }
      Utils::bytesToHex(hash, 32, Core::userConfig.passHashHex);
      memset(hash, 0, sizeof(hash));
    }
  }
  Storage::config.saveUserConfig();
  // audit-fixes: was a partial Indonesian/English mix. Now English-only.
  //   Indonesian translation should be in PWA, not in firmware responses.
  sendSuccess("Config saved. Please log in again with the new credentials.");
}

// POST /api/config/device { deviceName?, timezone?, requestId }
// PD-001 (Phase 6): REST ingress now uses the SHARED CommandCanonicalizer +
//   TransactionJournal path. Canonical mapping:
//     POST /api/config/device  →  type="config", action="setDevice"
inline void handleSetDeviceConfig() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(256)) return;
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
  if (doc.containsKey("deviceName")) {
    const char* dn = doc["deviceName"];
    if (dn && strlen(dn) > 0 && strlen(dn) <= 32) {
      // valid — will be applied below
    } else {
      sendError(400, "Device name must be 1-32 chars");
      return;
    }
  }
  if (doc.containsKey("timezone")) {
    const char* tz = doc["timezone"];
    if (tz && strlen(tz) >= 40) {
      sendError(400, "Timezone too long (max 39 chars)");
      return;
    }
  }

  // --- PD-001: Canonical command model integration ---
  // transactionId is OPTIONAL for /api/config/device (PWA does not yet send
  // requestId for this endpoint as of P2-2 reconciliation). When absent, the
  // command proceeds without journal integration (pre-PD-001 behavior).
  doc["type"] = "config";
  doc["action"] = "setDevice";

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
  if (doc.containsKey("deviceName")) {
    const char* dn = doc["deviceName"];
    if (dn && strlen(dn) > 0 && strlen(dn) <= 32) {
      strncpy(Core::deviceName, dn, 32);
      Core::deviceName[32] = '\0';
    }
  }
  if (doc.containsKey("timezone")) {
    const char* tz = doc["timezone"];
    if (tz && strlen(tz) < 40) {
      strncpy(Core::timezone, tz, 39);
      Core::timezone[39] = '\0';
    }
  }
  Storage::config.saveDeviceConfig();

  // --- Build success ACK JSON ---
  // Include requestId/commandHash only when present.
  String data = "{\"updated\":true,\"deviceName\":\"";
  data += Core::deviceName;
  data += "\",\"timezone\":\"";
  data += Core::timezone;
  data += "\"";
  if (tx.transactionId.length() > 0) {
    data += ",\"requestId\":\"";
    data += tx.transactionId;
    data += "\",\"commandHash\":\"";
    data += tx.commandHash;
    data += "\"";
  }
  data += "}";

  String ackJson = "{\"success\":true,\"message\":\"Device config updated\",\"data\":";
  ackJson += data;
  ackJson += "}";

  commitTransaction(tx.transactionId, tx.commandHash, ackJson);

  sendSecurityHeaders();
  Web::http.send(200, "application/json; charset=utf-8", ackJson);
}

// POST /api/config/password { current, next }
inline void handleChangePassword() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(256)) return;
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
  const char* current = doc["current"] | "";
  const char* next = doc["next"] | "";
  if (!Services::auth.changePassword(String(current), String(next))) {
    sendError(403, "Current password incorrect or new password too weak");
    return;
  }
  sendSuccess("Password changed", "{\"changed\":true}");
}

// GET /api/config/export → full backup JSON (downloadable)
inline void handleExportConfig() {
  if (!requireAuth()) return;
  String json = Storage::config.exportAll();
  Web::http.sendHeader("Content-Disposition",
                       "attachment; filename=\"timer12-config-backup.json\"");
  // audit-fixes: was `Access-Control-Allow-Origin: *` hardcoded — now uses
  //   shared helper so production builds enforce configured origin list.
  String origin = getAllowedOrigin();
  Web::http.sendHeader("X-Frame-Options", "DENY");
  if (origin.length() > 0) {
    Web::http.sendHeader("Access-Control-Allow-Origin", origin);
    Web::http.sendHeader("Access-Control-Allow-Credentials", "true");
    if (origin != "*") Web::http.sendHeader("Vary", "Origin");
  }
  Web::http.send(200, "application/json; charset=utf-8", json);
}

// POST /api/config/import  (body: full backup JSON)
inline void handleImportConfig() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(Core::MAX_BODY_SIZE)) return;
  if (!Web::http.hasArg("plain")) {
    sendError(400, "Missing body");
    return;
  }
  String body = Web::http.arg("plain");
  if (!Storage::config.importAll(body)) {
    sendError(400, "Invalid config (must have 12 channels)");
    return;
  }
  sendSuccess("Configuration imported", "{\"imported\":true}");
}

}} // namespace Web::Handlers

#endif
