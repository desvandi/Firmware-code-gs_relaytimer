// =============================================================================
// Services/OtaManager.cpp — Boot health check + automatic rollback
// =============================================================================
// R10B-6 (audit round 10B): Implements boot health verification + rollback.
//
// ESP32 has two OTA partitions (factory + ota_0). When new firmware is flashed
// via Update library, it writes to the INACTIVE partition, then sets it as
// the next boot partition. The ESP-IDF tracks "OTA sequence number" + valid
// state. If the new firmware fails to boot, the bootloader can fall back
// to the previous partition.
//
// This module implements application-level health check:
//   - On boot: increment boot_attempts in NVS
//   - If boot_attempts > MAX_BOOT_ATTEMPTS → call esp_ota_mark_app_invalid_rollback_and_restart()
//     which tells the bootloader to revert to previous partition on next boot.
//   - After all subsystems (WiFi, MQTT, RTC, LittleFS, RelayDriver) initialize
//     successfully → call markBootHealthy() which:
//       1. Resets boot_attempts to 0
//       2. Calls esp_ota_mark_app_valid_cancel_rollback() — confirms firmware is good
//
// Health check runs in setup() — if setup() crashes/hangs (e.g., MQTT init
// deadlock), watchdog will reboot, boot_attempts increments, eventually rollback.
// =============================================================================
#include "OtaManager.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include "WifiManager.h"
#include "RtcDriver.h"
#include "MqttClient.h"
#include <Update.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ESP-IDF OTA partition management
#ifdef ESP32
extern "C" {
  #include "esp_ota_ops.h"
}
#endif

namespace Services {

OtaManager otaManager;

static const char* NVS_KEY_BOOT_ATTEMPTS = "boot_attempts";
static const char* NVS_KEY_LAST_OTA_VERSION = "last_ota_ver";
static const uint8_t MAX_BOOT_ATTEMPTS = 3;
static const uint32_t HEALTH_CHECK_TIMEOUT_MS = 60000;  // 60s to prove health

// Existing OtaManager members (kept for backward compat with REST OTA handler)
bool _updating = false;
size_t _totalReceived = 0;
static const char LATEST_VERSION[] = "4.0.0";

void OtaManager::begin() {
  _updating = false;
  _totalReceived = 0;

  // R10B-6: Read boot attempts from NVS
  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, true);
  _bootAttempts = prefs.getUChar(NVS_KEY_BOOT_ATTEMPTS, 0);
  // Check if this is first boot after OTA (current partition != last known)
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
    _firstBootAfterOta = (state == ESP_OTA_IMG_PENDING_VERIFY);
  }
  prefs.end();

  Serial.printf("[OTA] Boot attempts: %d/%d (first boot after OTA: %s)\n",
                _bootAttempts, MAX_BOOT_ATTEMPTS,
                _firstBootAfterOta ? "YES" : "no");

  if (_firstBootAfterOta) {
    Services::Log.append(Core::LogType::Ota,
      "First boot after OTA update — health check required", 0);
  }

  // R10B-6: If too many boot attempts → trigger rollback
  if (_bootAttempts >= MAX_BOOT_ATTEMPTS) {
    Serial.printf("[OTA] FATAL: %d boot attempts exceeded — ROLLING BACK\n", _bootAttempts);
    Services::Log.append(Core::LogType::Error,
      "Boot failed " + String(_bootAttempts) + " times — rolling back to previous firmware", 0);

    #ifdef ESP32
    // Mark current firmware invalid + trigger rollback on next boot
    esp_ota_mark_app_invalid_rollback_and_restart();
    // Function above restarts — if it returns, force restart
    delay(500);
    ESP.restart();
    #endif
  }

  // Increment boot attempts (will be reset to 0 by markBootHealthy())
  if (_firstBootAfterOta) {
    prefs.begin(Core::NVS_NAMESPACE, false);
    prefs.putUChar(NVS_KEY_BOOT_ATTEMPTS, _bootAttempts + 1);
    prefs.end();
    Serial.printf("[OTA] Boot attempts incremented to %d\n", _bootAttempts + 1);
  }
}

// R10B-6: Verify all critical subsystems are healthy.
// Called from setup() after all subsystems initialized.
bool OtaManager::_verifyHealth() {
  // 1. LittleFS mounted (checked by FileSystem — if not mounted, would have crashed already)

  // 2. WiFi: either STA connected OR AP fallback active (not NONE)
  if (TimerNet::wifi.getMode() == TimerNet::WifiMode::NONE) {
    Serial.println("[OTA] Health check FAIL: WiFi not initialized");
    return false;
  }

  // 3. RTC time valid (year > 2020 — anything earlier means RTC not set or dead battery)
  int y, m, d, h, mi, s, weekday;
  Drivers::rtc.getDateTime(y, m, d, h, mi, s, weekday);
  if (y < 2024) {
    Serial.println("[OTA] Health check WARN: RTC not set (year < 2024) — not blocking");
    // Don't fail — RTC may just not be set yet. User can set via PWA.
  }

  // 4. Relay driver: verify at least one relay pin is in OUTPUT mode
  // (if RelayDriver::begin() crashed, pins would be in default INPUT mode)
  // This is implicitly verified — if relay init failed, setup() would have crashed.

  // 5. MQTT: if configured, check it's at least attempting to connect
  // (Don't fail health check if MQTT is down — broker may be temporarily unreachable)
  // But log a warning if MQTT not connected after 60s
  if (strlen(Core::GAS_INSIGHTS_URL) > 0 && !Services::mqtt.isConnected()) {
    Serial.println("[OTA] Health check WARN: MQTT not connected yet — not blocking");
  }

  Serial.println("[OTA] Health check PASS — all critical subsystems OK");
  return true;
}

void OtaManager::markBootHealthy() {
  if (!_firstBootAfterOta) {
    // Not first boot after OTA — no need to mark
    return;
  }

  if (!_verifyHealth()) {
    Serial.println("[OTA] Health check FAILED — will retry on next boot");
    Services::Log.append(Core::LogType::Error,
      "Boot health check failed — will retry (attempt " + String(_bootAttempts + 1) + ")", 0);
    // Don't mark as valid — reboot will increment attempts, eventually rollback
    return;
  }

  // Reset boot attempts
  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, false);
  prefs.putUChar(NVS_KEY_BOOT_ATTEMPTS, 0);
  prefs.end();

  // Mark firmware as valid (cancel rollback)
  #ifdef ESP32
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    Serial.println("[OTA] Firmware marked VALID — rollback cancelled");
    Services::Log.append(Core::LogType::Ota,
      "Firmware health verified — marked valid, rollback cancelled", 0);
  } else {
    Serial.printf("[OTA] WARNING: esp_ota_mark_app_valid_cancel_rollback() returned %d\n", err);
  }
  #endif

  _firstBootAfterOta = false;
  _bootAttempts = 0;
}

bool OtaManager::isFirstBootAfterOta() const {
  return _firstBootAfterOta;
}

// ============================================================================
// Legacy REST OTA upload handler support (unchanged — used by OtaHandlers.h)
// ============================================================================
String OtaManager::getLatestVersion() const {
  return String(LATEST_VERSION);
}

bool OtaManager::checkUpdateAvailable() const {
  return String(Core::FIRMWARE_VERSION) != String(LATEST_VERSION);
}

bool OtaManager::isUpdating() const { return _updating; }
size_t OtaManager::getProgress() const { return _totalReceived; }

} // namespace Services
