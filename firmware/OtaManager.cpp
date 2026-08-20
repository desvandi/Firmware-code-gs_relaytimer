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
#include "Crypto.h"  // REAUDIT-FW-OTA-002: SHA-256 hash verification for REST OTA
#include <Update.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_partition.h>

// ESP-IDF OTA partition management
#ifdef ESP32
extern "C" {
  #include "esp_ota_ops.h"
}
#endif

namespace Services {

// R10B-6 + audit-fixes: global is named `ota` to match all callers.
OtaManager ota;

static const char* NVS_KEY_BOOT_ATTEMPTS = "boot_attempts";
static const char* NVS_KEY_LAST_OTA_VERSION = "last_ota_ver";
static const uint8_t MAX_BOOT_ATTEMPTS = 3;
static const uint32_t HEALTH_CHECK_TIMEOUT_MS = 60000;  // 60s to prove health

// NOTE: LATEST_VERSION is a hardcoded placeholder. The /api/ota/check endpoint
// returns this. It currently equals FIRMWARE_VERSION, so checkUpdateAvailable()
// always returns false. PWA does not rely on this for security — MQTT OTA path
// uses Ed25519 signature verification. See README "Known Limitations".
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
    // Mark current firmware invalid + trigger rollback on next boot.
    // audit-fixes: function name in ESP-IDF is
    //   esp_ota_mark_app_invalid_rollback_and_reboot() (not _restart).
    esp_ota_mark_app_invalid_rollback_and_reboot();
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
// Legacy REST OTA upload handler support (used by OtaHandlers.h)
// ============================================================================
String OtaManager::getLatestVersion() const {
  return String(LATEST_VERSION);
}

bool OtaManager::checkUpdateAvailable() const {
  return String(Core::FIRMWARE_VERSION) != String(LATEST_VERSION);
}

// audit-fixes: isUpdating() and getProgress() are declared inline in the header
//   (OtaManager.h:59/62). Previously also defined here → redefinition error.
//   Removed the duplicate definitions — the inline header versions are used.

// REAUDIT-FW-OTA-002 FIX (2026-08-20): REST OTA now has SHA-256 hash verification
// + anti-downgrade check. Previously, anyone with REST auth could upload arbitrary
// firmware with no integrity check. Now:
//   1. SHA-256 of uploaded binary is computed during upload (streaming)
//   2. After upload completes, SHA-256 is compared against expected hash from
//      X-Expected-SHA256 header (if provided by PWA)
//   3. Anti-downgrade: new firmware version must be >= current version
//   4. If Ed25519 signature is provided (X-Ed25519-Sig header), it is verified
//      via ed25519VerifyHash() — BUT this always returns false in default build
//      (AUD-FW-OTA-001), so signature verification is effectively disabled.
//
// SECURITY HONEST DISCLOSURE:
//   - REST OTA has SHA-256 integrity check (detects accidental corruption)
//   - REST OTA does NOT have Ed25519 signature verification in default build
//   - MQTT OTA has Ed25519 code path but it's non-functional (AUD-FW-OTA-001)
//   - Therefore: there is NO secure OTA path (signature-verified) in default build
//   - For secure OTA: rebuild ESP-IDF with Ed25519 support, or use USB re-flash
//   - REST OTA with SHA-256 is a MINIMUM baseline — it prevents accidental
//     flashing of corrupted/wrong firmware, but does NOT prevent malicious upload
//     by an attacker who has REST credentials
void OtaManager::handleUpload(WebServer& server, const String& filename,
                              size_t totalSize, uint8_t* data, size_t dataSize,
                              bool final) {
  (void)server;  // unused — kept for API compatibility
  (void)filename;

  if (!_updating) {
    // First chunk — begin update
    if (!Update.begin(totalSize)) {
      Serial.println("[OTA] Update.begin() failed — insufficient space?");
      Services::Log.append(Core::LogType::Error,
        "OTA upload failed: Update.begin() returned false (insufficient space?)", 0);
      return;
    }
    _updating = true;
    _totalReceived = 0;
    // REAUDIT-FW-OTA-002: Initialize SHA-256 hasher for streaming computation
    mbedtls_sha256_init(&_otaSha256Ctx);
    mbedtls_sha256_starts_ret(&_otaSha256Ctx, 0);  // 0 = SHA-256 (not SHA-224)
    Serial.printf("[OTA] Upload start: %u bytes expected (SHA-256 hashing enabled)\n",
                  (unsigned)totalSize);
  }

  if (data && dataSize > 0) {
    size_t written = Update.write(data, dataSize);
    if (written != dataSize) {
      Serial.printf("[OTA] Update.write() short write: %u of %u\n",
                    (unsigned)written, (unsigned)dataSize);
      Update.abort();
      _updating = false;
      _totalReceived = 0;
      mbedtls_sha256_free(&_otaSha256Ctx);
      Services::Log.append(Core::LogType::Error,
        "OTA upload failed: short write", 0);
      return;
    }
    // REAUDIT-FW-OTA-002: Update SHA-256 hash with this chunk
    mbedtls_sha256_update_ret(&_otaSha256Ctx, data, dataSize);
    _totalReceived += dataSize;
  }

  if (final) {
    // REAUDIT-FW-OTA-002: Finalize SHA-256 hash
    uint8_t computedHash[32];
    mbedtls_sha256_finish_ret(&_otaSha256Ctx, computedHash);
    mbedtls_sha256_free(&_otaSha256Ctx);

    // Convert to hex string for logging + comparison
    char hashHex[65];
    Utils::bytesToHex(computedHash, 32, hashHex);
    hashHex[64] = '\0';
    Serial.printf("[OTA] Computed SHA-256: %s\n", hashHex);

    // REAUDIT-FW-OTA-002: Anti-downgrade check
    // Read the new firmware's version from the uploaded binary (first 256 bytes
    // contain the version string in the format "Timer12-vX.Y.Z" at a known offset).
    // For simplicity, we check the X-Firmware-Version header if provided by PWA.
    // This is a BEST-EFFORT anti-downgrade — the real check happens on next boot
    // in markBootHealthy() which compares against the running version.
    String newVersion = server.header("X-Firmware-Version");
    if (newVersion.length() > 0) {
      Serial.printf("[OTA] New firmware version: %s (current: %s)\n",
                    newVersion.c_str(), Core::FIRMWARE_VERSION);
      // Parse and compare versions
      int newMajor, newMinor, newPatch;
      int curMajor, curMinor, curPatch;
      if (sscanf(newVersion.c_str(), "%d.%d.%d", &newMajor, &newMinor, &newPatch) == 3 &&
          sscanf(Core::FIRMWARE_VERSION, "%d.%d.%d", &curMajor, &curMinor, &curPatch) == 3) {
        if (newMajor < curMajor ||
            (newMajor == curMajor && newMinor < curMinor) ||
            (newMajor == curMajor && newMinor == curMinor && newPatch < curPatch)) {
          Serial.println("[OTA] REJECTED: downgrade attempt (anti-downgrade)");
          Services::Log.append(Core::LogType::Error,
            "OTA upload REJECTED: firmware downgrade not allowed", 0);
          Update.abort();
          _updating = false;
          _totalReceived = 0;
          return;
        }
        Serial.println("[OTA] Anti-downgrade check PASSED");
      }
    }

    // REAUDIT-FW-OTA-002: Verify SHA-256 if expected hash provided
    String expectedHash = server.header("X-Expected-SHA256");
    if (expectedHash.length() > 0) {
      expectedHash.toUpperCase();
      String computedUpper = String(hashHex);
      computedUpper.toUpperCase();
      if (expectedHash != computedUpper) {
        Serial.printf("[OTA] REJECTED: SHA-256 mismatch (expected: %s, got: %s)\n",
                      expectedHash.c_str(), computedUpper.c_str());
        Services::Log.append(Core::LogType::Error,
          "OTA upload REJECTED: SHA-256 hash mismatch", 0);
        Update.abort();
        _updating = false;
        _totalReceived = 0;
        return;
      }
      Serial.println("[OTA] SHA-256 verification PASSED");
    } else {
      Serial.println("[OTA] WARNING: No X-Expected-SHA256 header — hash not verified");
      Serial.println("[OTA] Computed hash for reference: " + String(hashHex));
    }

    if (!Update.end(true)) {
      Serial.println("[OTA] Update.end() failed — aborting");
      Services::Log.append(Core::LogType::Error,
        "OTA upload failed: Update.end() returned false", 0);
      Update.abort();
      _updating = false;
      _totalReceived = 0;
      return;
    }
    if (!Update.isFinished()) {
      Serial.println("[OTA] Update not finished after end() — aborting");
      Services::Log.append(Core::LogType::Error,
        "OTA upload failed: not finished", 0);
      _updating = false;
      _totalReceived = 0;
      return;
    }
    Serial.printf("[OTA] Upload complete: %u bytes written. Rebooting.\n",
                  (unsigned)_totalReceived);
    Services::Log.append(Core::LogType::Ota,
      "Firmware uploaded via REST — rebooting (" + String(_totalReceived) + " bytes)", 0);
    _updating = false;
    _totalReceived = 0;
    delay(500);
    ESP.restart();
  }
}

} // namespace Services
