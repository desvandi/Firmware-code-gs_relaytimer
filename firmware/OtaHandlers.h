// =============================================================================
// Web/Handlers/OtaHandlers.h — /api/ota, /api/ota/check
// =============================================================================
// audit-fixes-v2 (auditor #4 P0-1):
//   REST OTA upload path (POST /api/ota multipart) was a P0 release blocker.
//   The ESP32 WebServer library calls the upload handler for EACH multipart
//   chunk BEFORE the response handler runs. The response handler had
//   requireAuth(), but the upload handler did NOT — so unauthenticated
//   attackers could write arbitrary bytes to the OTA partition via
//   Update.write() before the auth check ran.
//
//   MQTT OTA (the signed path via Ed25519) remains the production-grade path.
//   REST OTA upload is now HARD-DISABLED in PRODUCTION_BUILD:
//     - handleOtaUpload() rejects all chunks with 403 + aborts Update
//     - handleOtaResponse() rejects with 403 ("use MQTT OTA")
//   In development builds, REST OTA upload still works for convenience
//   (LAN dev only — not exposed to internet).
#pragma once
#ifndef TIMER12_WEB_HANDLERS_OTA_H
#define TIMER12_WEB_HANDLERS_OTA_H

#include <Arduino.h>
#include "Common.h"
#include "OtaManager.h"
#include "AuthManager.h"
#include "LogService.h"
#include "Config.h"

namespace Web { namespace Handlers {

// POST /api/ota (multipart upload of .bin file)
//   WebServer calls handleOtaUpload for each chunk, then handleOtaResponse at end
inline void handleOtaResponse() {
  if (!requireAuth()) return;
#ifdef PRODUCTION_BUILD
  // audit-fixes-v2 (P0-1): REST OTA upload is disabled in production.
  //   Use MQTT OTA (Ed25519 signed) for firmware updates in production.
  //   The upload handler below also short-circuits all chunks, but we
  //   also reject the response phase as defense-in-depth.
  sendError(403, "REST OTA disabled in production — use MQTT OTA (Ed25519 signed)");
  return;
#else
  // Development mode: REST OTA works for LAN-only dev convenience.
  sendSuccess("OTA complete", "{\"success\":true}");
#endif
}

inline void handleOtaUpload() {
  HTTPUpload& upload = Web::http.upload();
#ifdef PRODUCTION_BUILD
  // audit-fixes-v2 (P0-1): refuse all upload chunks in production.
  //   This is the GATE that was missing — even if the response handler
  //   had auth, the upload handler is called per-chunk BEFORE the response
  //   handler. Without this gate, Update.write() runs on attacker bytes.
  //   On the FIRST chunk (UPLOAD_FILE_START), abort immediately. Subsequent
  //   chunks are ignored because Update.begin() was never called.
  if (upload.status == UPLOAD_FILE_START) {
    Services::Log.append(Core::LogType::AuthFail,
      "REST OTA upload REJECTED in production (unauthenticated path disabled)", 0);
    Web::http.send(403, "application/json",
      "{\"success\":false,\"message\":\"REST OTA disabled in production\",\"data\":null}");
  }
  // For subsequent chunks: do nothing. Update was never begun, so there's
  // nothing to write. The 403 above will terminate the connection.
  return;
#else
  // Development mode: process upload chunks normally.
  //   NOTE: still requires auth via handleOtaResponse() at end of upload.
  //   For dev-only LAN use — never expose to internet.
  if (upload.status == UPLOAD_FILE_START) {
    Services::ota.handleUpload(Web::http, upload.filename, 0, nullptr, 0, false);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Services::ota.handleUpload(Web::http, upload.filename, upload.totalSize,
                               upload.buf, upload.currentSize, false);
  } else if (upload.status == UPLOAD_FILE_END) {
    Services::ota.handleUpload(Web::http, upload.filename, upload.totalSize,
                               upload.buf, upload.currentSize, true);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Services::Log.append(Core::LogType::Error, "OTA upload aborted", 0);
    sendError(500, "OTA aborted");
  }
#endif
}

// POST /api/ota/check → query GitHub Release for latest version
inline void handleOtaCheck() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  String latest = Services::ota.getLatestVersion();
  bool available = Services::ota.checkUpdateAvailable();
  String data = "{\"available\":";
  data += available ? "true" : "false";
  data += ",\"latestVersion\":\"";
  data += latest;
  data += "\",\"currentVersion\":\"";
  data += Core::FIRMWARE_VERSION;
  data += "\"}";
  sendSuccess(available ? "Update available" : "Firmware is up to date", data);
}

}} // namespace Web::Handlers

#endif

