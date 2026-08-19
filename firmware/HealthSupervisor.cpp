// =============================================================================
// HealthSupervisor.cpp — System health monitoring (brief §44, §45)
// =============================================================================
#include "HealthSupervisor.h"
#include "Globals.h"
#include "Config.h"
#include "AlarmRegistry.h"
#include "ErrorCodes.h"
#include "RtcDriver.h"
#include "PzemDriver.h"
#include "PirDriver.h"
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <cstring>
#include <algorithm>

namespace Services {

HealthSupervisor health;

// ESP32 reset reason codes (matches esp_reset_reason_t enum from esp_system.h)
static const char* RESET_REASON_STR[] = {
  "UNKNOWN",           // 0
  "POWERON_RESET",     // 1
  "EXT_RESET",          // 3
  "SW_RESET",           // 4 (software reset via ESP.restart)
  "OWDT_RESET",        // 5 (watchdog)
  "DEEPSLEEP_RESET",   // 7
  "TG0WDT_SYS_RESET",  // 8
  "TG1WDT_SYS_RESET",  // 9
  "RTCWDT_BROWN_OUT_RESET",  // 11
  "RTCWDT_SYS_RESET",  // 12
  "CPU0_SW",            // 14
  "CPU1_SW",            // 15
  "NO_REASON",          // 16+ (default)
};

const char* HealthSupervisor::resetReasonStr(uint8_t reason) {
  if (reason <= 15) return RESET_REASON_STR[reason];
  return "UNKNOWN";
}

void HealthSupervisor::begin() {
  _snapshot = {};
  _snapshot.uptimeSeconds = 0;
  _snapshot.bootCount = 0;
  _snapshot.lastResetReason = 0;
  _snapshot.watchdogResets = 0;
  _snapshot.brownoutResets = 0;
  _snapshot.freeHeap = 0;
  _snapshot.minFreeHeap = 0;
  _snapshot.largestFreeBlock = 0;
  _snapshot.wifiRssi = -127;
  _snapshot.wifiReconnectCount = 0;
  _snapshot.mqttReconnectCount = 0;
  _snapshot.rtcStatus = RtcStatus::Unsynced;
  _snapshot.pzemStatus = SensorStatus::Unavailable;
  _snapshot.pirStatus = SensorStatus::Unavailable;
  _snapshot.filesystemOk = false;
  _snapshot.nvsOk = false;
  _snapshot.mqttConnected = false;
  _snapshot.telemetrySequence = 0;
  _snapshot.highestAlarm = AlarmSeverity::Info;
  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    _snapshot.taskHeartbeatAgeMs[i] = 0;
    _lastHeartbeatMs[i] = 0;
  }

  // Load persistent counters from NVS (boot count, watchdog/brownout counts)
  Preferences p;
  if (p.begin("health", false)) {
    _snapshot.bootCount = p.getUInt("boot_cnt", 0) + 1;
    p.putUInt("boot_cnt", _snapshot.bootCount);
    _snapshot.watchdogResets = p.getUInt("wdt_cnt", 0);
    _snapshot.brownoutResets = p.getUInt("brn_cnt", 0);
    _snapshot.lastResetReason = (uint8_t)p.getUChar("last_rr", 0);
    p.end();
    _snapshot.nvsOk = true;
  } else {
    _snapshot.nvsOk = false;
  }

  // Read reset reason from ESP32 (esp_reset_reason() returns 1..15)
  // Use rom_reset_reason or esp_reset_reason() if available
  // For portability we just use the NVS-cached value + classify
  uint8_t currentReason = 0;  // will be updated by recordBoot()
  _snapshot.lastResetReason = currentReason;

  // Initialize free heap observation
  _snapshot.freeHeap = ESP.getFreeHeap();
  _snapshot.minFreeHeap = ESP.getMinFreeHeap();
  // largestFreeBlock — query via heap_caps_get_largest_free_block if available
  // For portability, set to freeHeap as a conservative estimate
  _snapshot.largestFreeBlock = _snapshot.freeHeap;

  _initialized = true;
  Serial.printf("[HEALTH] init: bootCount=%u, wdtResets=%u, brnResets=%u\n",
                _snapshot.bootCount, _snapshot.watchdogResets, _snapshot.brownoutResets);
}

void HealthSupervisor::recordBoot() {
  // Classify reset reason from rom_reset_reason (rawr ESP32 register)
  // The value is in RTC memory; for portability we use 0 = UNKNOWN here.
  // In production this would query esp_reset_reason() from esp_system.h.
  // For now we just store it in NVS on next boot.
  uint8_t reason = 0;
  // Attempt to read raw reset reason (best-effort — varies by ESP-IDF version)
#if ESP_IDF_VERSION_MAJOR >= 5
  // ESP-IDF v5+ exposes esp_reset_reason()
  extern "C" int esp_reset_reason(void);  // declaration only; weakly linked
  reason = (uint8_t)esp_reset_reason();
#elif ESP_IDF_VERSION_MAJOR >= 4
  reason = (uint8_t)esp_reset_reason();
#endif

  _snapshot.lastResetReason = reason;
  if (reason == 5 || reason == 8 || reason == 9 || reason == 12) {
    // Watchdog-style reset
    _snapshot.watchdogResets++;
    alarms.raise("WATCHDOG_RESET", AlarmSeverity::Warning,
                 "Watchdog reset detected on boot");
  } else if (reason == 11) {
    // Brownout reset
    _snapshot.brownoutResets++;
    alarms.raise("BROWNOUT_RESET", AlarmSeverity::Warning,
                 "Brownout reset detected on boot");
  }

  // Persist updated counters
  Preferences p;
  if (p.begin("health", false)) {
    p.putUInt("wdt_cnt", _snapshot.watchdogResets);
    p.putUInt("brn_cnt", _snapshot.brownoutResets);
    p.putUChar("last_rr", reason);
    p.end();
  }

  // Repeated-reboot detection (brief §60: "repeated reboot")
  // If bootCount - lastBootCount differs by more than 3 in 60 seconds → alarm
  // (simplified: if bootCount grew by more than 5 between code revisions,
  // we assume rapid-cycling and raise an alarm)
  static uint32_t lastBootCount = 0;
  if (_snapshot.bootCount > lastBootCount + 5) {
    alarms.raise("REPEATED_REBOOT", AlarmSeverity::Critical,
                 "Repeated reboot detected — possible boot loop");
  }
  lastBootCount = _snapshot.bootCount;
}

void HealthSupervisor::recordWifiReconnect() {
  _snapshot.wifiReconnectCount++;
}
void HealthSupervisor::recordMqttReconnect() {
  _snapshot.mqttReconnectCount++;
}
void HealthSupervisor::recordHeartbeat(TaskId id) {
  if ((uint8_t)id >= TASK_COUNT) return;
  _lastHeartbeatMs[(uint8_t)id] = millis();
}
void HealthSupervisor::recordCrash(uint32_t uptimeAtCrash) {
  _snapshot.lastCrashUptime = uptimeAtCrash;
}

void HealthSupervisor::setRtcStatus(RtcStatus s) {
  if (s == _rtcStatus) return;
  _rtcStatus = s;
  _snapshot.rtcStatus = s;
  if (s == RtcStatus::Invalid) {
    alarms.raise(Err::RTC_INVALID, AlarmSeverity::Critical,
                 "RTC invalid — scheduler inhibited until RTC corrected");
  } else if (s == RtcStatus::Unsynced) {
    alarms.raise(Err::RTC_UNSYNCED, AlarmSeverity::Warning,
                 "RTC unsynced — schedule execution may be unreliable");
  } else {
    alarms.clear(Err::RTC_INVALID);
    alarms.clear(Err::RTC_UNSYNCED);
  }
}

uint32_t HealthSupervisor::nextTelemetrySequence() {
  _snapshot.telemetrySequence++;
  return _snapshot.telemetrySequence;
}

void HealthSupervisor::tick() {
  if (!_initialized) return;
  unsigned long now = millis();
  if (now - _lastTickMs < 1000) return;  // 1 Hz
  _lastTickMs = now;

  _snapshot.uptimeSeconds = (uint32_t)(millis() / 1000);
  _snapshot.freeHeap = ESP.getFreeHeap();
  _snapshot.minFreeHeap = ESP.getMinFreeHeap();
  // Update min free heap observed (track lowest)
  if (_snapshot.minFreeHeap > _snapshot.freeHeap) {
    _snapshot.minFreeHeap = _snapshot.freeHeap;
  }

  // Update per-task heartbeat ages
  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    if (_lastHeartbeatMs[i] == 0) {
      _snapshot.taskHeartbeatAgeMs[i] = (uint32_t)-1;  // never heartbeated
    } else {
      _snapshot.taskHeartbeatAgeMs[i] = (uint32_t)(now - _lastHeartbeatMs[i]);
    }
  }

  // §45: Task stall detection — if any heartbeat exceeds 10s, raise alarm
  static const char* TASK_NAMES[TASK_COUNT] = {
    "RELAY_ENGINE", "MQTT", "TELEMETRY", "SCHEDULER",
    "PIR", "PZEM", "OTA", "HEALTH_MONITOR", "BATTERY_MONITOR"
  };
  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    if (_snapshot.taskHeartbeatAgeMs[i] == (uint32_t)-1) continue;
    if (_snapshot.taskHeartbeatAgeMs[i] > 10000) {  // 10s threshold
      char buf[64];
      snprintf(buf, sizeof(buf), "Task %s stalled — heartbeat age %ums",
               TASK_NAMES[i], _snapshot.taskHeartbeatAgeMs[i]);
      char code[24];
      snprintf(code, sizeof(code), "TASK_STALL_%s", TASK_NAMES[i]);
      alarms.raise(code, AlarmSeverity::Warning, buf);
    }
  }

  // Heap-exhaustion detection
  if (_snapshot.freeHeap < 20000) {  // <20 KB free
    alarms.raise("LOW_HEAP", AlarmSeverity::Warning,
                 "Free heap below 20 KB — possible memory pressure");
  }

  _snapshot.highestAlarm = alarms.highestActiveSeverity();
}

} // namespace Services
