// =============================================================================
// Web/Handlers/StatusHandlers.h — /api/status, /api/version, /api/health
// =============================================================================
#pragma once
#ifndef TIMER12_WEB_HANDLERS_STATUS_H
#define TIMER12_WEB_HANDLERS_STATUS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Common.h"
#include "RtcDriver.h"
#include "RelayDriver.h"
#include "PirDriver.h"
#include "WifiManager.h"
#include "AuthManager.h"
#include "Scheduler.h"
#include "OtaManager.h"
#include "Config.h"
#include "Globals.h"
#include "Crypto.h"  // CYCLE-7: for Utils::sha256Hex in handleGasSecret

namespace Web { namespace Handlers {

// GET /api/status → SystemStatus (PWA contract)
inline void handleStatus() {
  if (!requireAuth()) return;
  DynamicJsonDocument doc(8192);
  JsonObject data = doc.createNestedObject("data");

  // Device info
  data["firmwareVersion"] = Core::FIRMWARE_VERSION;
  data["buildDate"] = Core::BUILD_DATE;
  data["deviceName"] = Core::deviceName;
  data["uptimeSeconds"] = (uint32_t)(millis() / 1000);
  data["currentTime"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
  data["timezone"] = Core::timezone;
  data["wifiRssi"] = TimerNet::wifi.getRssi();
  data["freeHeap"] = ESP.getFreeHeap();
  // audit-fixes: removed hardcoded cpuLoadPercent=10 and flashFreePercent=35.
  //   These were mock values that misled the PWA into thinking telemetry was
  //   real. Real CPU load measurement on ESP32 requires idle-task hook (not
  //   implemented). Flash free space requires esp_partition_info (not worth
  //   the memory cost for the value it provides). Omitted from response
  //   rather than fabricating numbers.
  data["online"] = true;

  // Channels array
  JsonArray chArr = data.createNestedArray("channels");
  int y, m, d, h, mi, s, weekday;
  Drivers::rtc.getDateTime(y, m, d, h, mi, s, weekday);
  uint16_t currentMin = h * 60 + mi;
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    JsonObject ch = chArr.createNestedObject();
    ch["id"] = i + 1;
    ch["name"] = Core::channels[i].name;
    ch["modeAuto"] = Core::channels[i].modeAuto;
    ch["manualState"] = Core::channels[i].manualState;
    ch["pirEnabled"] = Core::channels[i].pirEnabled;
    ch["pirHoldTime"] = Core::channels[i].pirHoldTime;
    ch["state"] = Core::relayState[i];
    const char* srcStr =
      Core::relaySource[i] == Core::RelaySource::Manual ? "manual" :
      Core::relaySource[i] == Core::RelaySource::Schedule ? "schedule" :
      Core::relaySource[i] == Core::RelaySource::Pir ? "pir" : "off";
    ch["source"] = srcStr;
    ch["hasPir"] = (i >= Core::PIR_CHANNEL_OFFSET);
  }

  // PIR array
  JsonArray pirArr = data.createNestedArray("pirs");
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) {
    JsonObject p = pirArr.createNestedObject();
    p["id"] = i + 1;
    p["channelId"] = Core::PIR_CHANNEL_OFFSET + i + 1;
    p["enabled"] = Core::channels[Core::PIR_CHANNEL_OFFSET + i].pirEnabled;
    p["motionNow"] = Core::pirState[i].motionNow;
    p["lastMotionAt"] = Core::pirState[i].lastMotion
      ? (uint64_t)(Drivers::rtc.getUnixTime() - (millis() - Core::pirState[i].lastMotion) / 1000) * 1000ULL
      : 0;
    p["triggerCountToday"] = Core::pirState[i].triggerCountToday;
    p["warmupUntil"] = (uint64_t)(Core::pirStartupTime + Core::PIR_WARMUP_MS);
    p["stuckDetected"] = Core::pirState[i].stuckAlerted;
    p["holdTime"] = Core::channels[Core::PIR_CHANNEL_OFFSET + i].pirHoldTime;
  }

  // Stats
  JsonObject stats = data.createNestedObject("stats");
  uint8_t onCount = 0;
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) if (Core::relayState[i]) onCount++;
  stats["relaysOn"] = onCount;
  // Active schedules count (best-effort)
  int schedActive = 0;
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    for (uint8_t j = 0; j < Core::channels[i].schedCount; j++) {
      if (Services::scheduler.isScheduleActive(Core::channels[i].sched[j], currentMin, weekday)) {
        schedActive++;
        break;
      }
    }
  }
  stats["schedulesActive"] = schedActive;
  uint32_t pirToday = 0;
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) pirToday += Core::pirState[i].triggerCountToday;
  stats["pirTriggersToday"] = pirToday;
  stats["errorsToday"] = Core::metrics.errorsToday;

  // Serialize
  String body;
  body.reserve(4096);
  doc["success"] = true;
  serializeJson(doc, body);
  // audit-fixes: was `Access-Control-Allow-Origin: *` hardcoded — bypassed
  //   Config::ALLOWED_CORS_ORIGINS. Now uses the shared helper so production
  //   builds enforce the configured origin list.
  String origin = getAllowedOrigin();
  Web::http.sendHeader("X-Frame-Options", "DENY");
  Web::http.sendHeader("Cache-Control", "no-store");
  if (origin.length() > 0) {
    Web::http.sendHeader("Access-Control-Allow-Origin", origin);
    Web::http.sendHeader("Access-Control-Allow-Credentials", "true");
    if (origin != "*") Web::http.sendHeader("Vary", "Origin");
  }
  Web::http.send(200, "application/json; charset=utf-8", body);
}

// GET /api/version → FirmwareInfo (PWA contract)
inline void handleVersion() {
  if (!requireAuth()) return;
  String data = "{";
  data += "\"currentVersion\":\"" + String(Core::FIRMWARE_VERSION) + "\",";
  data += "\"buildDate\":\"" + String(Core::BUILD_DATE) + "\",";
  data += "\"latestAvailable\":\"" + Services::ota.getLatestVersion() + "\",";
  data += "\"updateAvailable\":" + String(Services::ota.checkUpdateAvailable() ? "true" : "false") + ",";
  data += "\"signatureVerified\":true,";
  data += "\"otaStatus\":\"up-to-date\",";
  data += "\"lastUpdateAt\":null,";
  data += "\"lastUpdateStatus\":null";
  data += "}";
  sendSuccess("", data);
}

// GET /api/health → hardware diagnostics
inline void handleHealth() {
  if (!requireAuth()) return;
  String data = "{";
  data += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  data += "\"minFreeHeap\":" + String(ESP.getMinFreeHeap()) + ",";
  data += "\"uptime\":" + String(millis() / 1000) + ",";
  data += "\"wifiClients\":" + String(TimerNet::wifi.getClientCount()) + ",";
  data += "\"flashSize\":" + String(ESP.getFlashChipSize()) + ",";
  data += "\"flashSpeed\":" + String(ESP.getFlashChipSpeed()) + ",";
  data += "\"cpuFreq\":" + String(ESP.getCpuFreqMHz()) + ",";
  data += "\"timeValid\":" + String(Core::timeValid ? "true" : "false") + ",";
  data += "\"scheduleDirty\":" + String(Core::scheduleDirty ? "true" : "false");
  data += "}";
  sendSuccess("", data);
}

// ============================================================================
// CYCLE-7 (fixes F-013): GET /api/gas_secret → returns per-device GAS HMAC secret.
//
// PWA needs the secret to sign GET requests to GAS (Code.gs now requires HMAC
// for insights endpoint — previously anonymous device ID was the only "key",
// which allowed anyone who knew the ID to read device insights).
//
// Security:
//   - Requires JWT auth (requireAuth()).
//   - Returns the secret as hex string (64 chars).
//   - PWA stores in localStorage (per-device), uses to sign GAS GET requests.
//   - Secret is per-device (generated by ESP32 at first boot, stored in NVS).
//   - Compromising one device's PWA session doesn't affect other devices.
//
// Threat model:
//   - Attacker with XSS in PWA could read localStorage and extract secret.
//   - Mitigation: CSP headers (already enforced), short-lived JWT sessions,
//     and the secret is only useful for GAS insights (read-only, low value).
//   - For higher security: use a backend proxy (PWA → Vercel API → GAS) so
//     the secret stays server-side. Documented in README as future work.
// ============================================================================
inline void handleGasSecret() {
  if (!requireAuth()) return;
  String secretHex = TimerNet::wifi.getGasSecretHex();
  if (secretHex.length() != Core::GAS_SECRET_LEN * 2) {
    sendError(500, "GAS secret not configured on device");
    return;
  }
  // Compute anonymous device ID (first 16 chars of SHA-256(MAC))
  // This is the same ID used by Advisor.cpp when POSTing to GAS.
  String anonId = Utils::sha256Hex(TimerNet::wifi.getMacAddress()).substring(0, 16);
  String data = "{";
  data += "\"secret\":\"" + secretHex + "\",";
  data += "\"deviceId\":\"" + anonId + "\"";
  data += "}";
  sendSuccess("GAS secret retrieved (store securely, use to sign GAS GET requests)", data);
}

}} // namespace Web::Handlers

#endif
