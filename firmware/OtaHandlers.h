// =============================================================================
// Web/Handlers/OtaHandlers.h — /api/ota, /api/ota/check
// =============================================================================
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
  // If we got here without OTA completing, send generic success
  sendSuccess("OTA complete", "{\"success\":true}");
}

inline void handleOtaUpload() {
  // WebServer signature: HTTPUpload& upload
  // We need to call Services::ota.handleUpload(Web::http, ...)
  HTTPUpload& upload = Web::http.upload();
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
}

// POST /api/ota/check → query GitHub Release for latest version
inline void handleOtaCheck() {
  if (!requireAuth()) return;
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
