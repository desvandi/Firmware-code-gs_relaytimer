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

private:
  uint8_t _bootAttempts = 0;
  bool _firstBootAfterOta = false;

  void _checkRollback();
  bool _verifyHealth();
};

extern OtaManager otaManager;

} // namespace Services

#endif
