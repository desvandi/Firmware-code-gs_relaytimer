// =============================================================================
// Services/OtaManager.h — OTA firmware update + boot health check + rollback
// =============================================================================
// R10B-6 (audit round 10B): Boot health check + automatic rollback.
//
// Flow:
//   1. Firmware boots → increment boot_attempts in NVS
//   2. setup() runs — if boot_attempts > 3 → mark firmware INVALID
//      → ESP32 anti-rollback (partition swap on next boot)
//   3. If setup() completes successfully → call markBootHealthy()
//      → resets boot_attempts to 0, marks firmware VALID
//
// Health criteria (checked in markBootHealthy()):
//   - WiFi connected (STA or AP fallback)
//   - MQTT connected (if configured)
//   - RTC time valid (year > 2020)
//   - LittleFS mounted
//   - Relay driver initialized
//
// If any fails within 60s of boot → reboot → boot_attempts increments
// → after 3 attempts, firmware marked invalid → rollback.
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_OTA_MANAGER_H
#define TIMER12_SERVICES_OTA_MANAGER_H

#include <Arduino.h>
#include <WebServer.h>

namespace Services {

class OtaManager {
public:
  // Called early in setup() — increments boot_attempts counter in NVS.
  // If boot_attempts exceeds MAX_BOOT_ATTEMPTS → trigger rollback.
  void begin();

  // Called after all subsystems initialized successfully (end of setup()).
  // Resets boot_attempts to 0, marks current firmware as healthy.
  void markBootHealthy();

  // Returns true if current boot is the first boot after OTA update.
  bool isFirstBootAfterOta() const;

  // Returns current boot attempt count (0 = healthy, >0 = retrying).
  uint8_t getBootAttempts() const { return _bootAttempts; }

  // -------- REST OTA upload (used by OtaHandlers.h) --------
  // Returns the latest firmware version string (for /api/ota/check).
  // NOTE: currently a hardcoded constant — real GitHub Release check is not
  // implemented. PWA should not rely on this for security decisions.
  String getLatestVersion() const;

  // Returns true if a newer firmware than current is available.
  // NOTE: currently always false because LATEST_VERSION == FIRMWARE_VERSION.
  bool checkUpdateAvailable() const;

  // True while a REST OTA upload is in progress (blocks concurrent updates).
  bool isUpdating() const { return _updating; }

  // Bytes received so far for the current REST OTA upload.
  size_t getProgress() const { return _totalReceived; }

  // REST OTA upload chunk handler — called by ESP32 WebServer for each
  // multipart chunk of the .bin upload. Signs nothing — REST OTA is
  // only available on LAN mode (HTTPS-tunnelled) and requires JWT auth.
  // For MQTT OTA (the signed path), see MqttClient::_handleOta().
  void handleUpload(WebServer& server, const String& filename,
                    size_t totalSize, uint8_t* data, size_t dataSize,
                    bool final);

private:
  uint8_t _bootAttempts = 0;
  bool _firstBootAfterOta = false;
  bool _updating = false;
  size_t _totalReceived = 0;

  bool _verifyHealth();
};

// NOTE: extern global is named `ota` (not `otaManager`) — matches all caller
// references in firmware_v4.ino, OtaHandlers.h, and StatusHandlers.h.
extern OtaManager ota;

} // namespace Services

#endif
