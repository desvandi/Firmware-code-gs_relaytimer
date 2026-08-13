// =============================================================================
// Services/OtaManager.cpp
// =============================================================================
#include "OtaManager.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include <Update.h>
#include <esp_task_wdt.h>

namespace Services {

OtaManager ota;

// Mock latest version — in production, fetch from GitHub Release API
//   https://api.github.com/repos/<you>/timer-relay-firmware/releases/latest
// and verify the .sig file with the public key burned into NVS.
static const char LATEST_VERSION[] = "4.0.0";

void OtaManager::begin() {
  _updating = false;
  _totalReceived = 0;
}

String OtaManager::getLatestVersion() const {
  return String(LATEST_VERSION);
}

bool OtaManager::checkUpdateAvailable() const {
  return String(Core::FIRMWARE_VERSION) != String(LATEST_VERSION);
}

void OtaManager::handleUpload(WebServer& server, String filename, size_t index,
                               uint8_t* data, size_t len, bool final) {
  if (!index) {
    // Start of upload
    if (!Update.begin(Core::OTA_MAX_BINARY_SIZE)) {
      Services::Log.append(Core::LogType::Ota, "OTA begin failed", 0);
      server.send(500, "application/json",
                  "{\"success\":false,\"message\":\"OTA begin failed\",\"data\":null}");
      _updating = false;
      return;
    }
    _updating = true;
    _totalReceived = 0;
    _startTime = millis();
    Services::Log.append(Core::LogType::Ota,
      "OTA update started: " + filename, 0);
  }
  if (_updating) {
    if (len > 0) {
      size_t written = Update.write(data, len);
      if (written != len) {
        Services::Log.append(Core::LogType::Error, "OTA write mismatch", 0);
        Update.abort();
        _updating = false;
        server.send(500, "application/json",
                    "{\"success\":false,\"message\":\"OTA write failed\",\"data\":null}");
        return;
      }
      _totalReceived += len;
      esp_task_wdt_reset();
    }
    if (final) {
      if (Update.end(true)) {
        if (Update.isFinished()) {
          uint32_t dur = (millis() - _startTime) / 1000;
          char msg[64];
          snprintf(msg, sizeof(msg), "OTA success %u bytes in %us",
                   (unsigned)_totalReceived, (unsigned)dur);
          Services::Log.append(Core::LogType::Ota, msg, 0);
          server.sendHeader("Connection", "close");
          String resp = "{\"success\":true,\"message\":\"OTA update successful. Rebooting.\",\"data\":{\"newVersion\":\"";
          resp += LATEST_VERSION;
          resp += "\"}}";
          server.send(200, "application/json", resp);
          // Schedule reboot
          delay(500);
          ESP.restart();
        } else {
          Services::Log.append(Core::LogType::Error, "OTA not finished", 0);
          server.send(500, "application/json",
                      "{\"success\":false,\"message\":\"OTA not finished\",\"data\":null}");
        }
      } else {
        Services::Log.append(Core::LogType::Error, "OTA end failed", 0);
        server.send(500, "application/json",
                    "{\"success\":false,\"message\":\"OTA failed - rollback\",\"data\":null}");
      }
      _updating = false;
    }
  }
}

} // namespace Services
