// =============================================================================
// Core/Types.h — Data structures shared across modules
// Timer Digital Relay v4.0
// =============================================================================
#pragma once
#ifndef TIMER12_CORE_TYPES_H
#define TIMER12_CORE_TYPES_H

#include <Arduino.h>
#include <cstdint>
#include "Config.h"

namespace Core {

// ---------- SCHEDULE ----------
struct Schedule {
  char onTime[MAX_TIME_BUF];    // "HH:MM"
  char offTime[MAX_TIME_BUF];   // "HH:MM"
  uint16_t onMin;               // precomputed minutes since midnight
  uint16_t offMin;
  uint8_t dayMask;              // bit0=Mon ... bit6=Sun; 0 = every day
  bool enabled;                 // v4.0: per-schedule enable flag
};

// ---------- CHANNEL ----------
// v4.2 audit (brief §13-16): per-channel safety limits + boot policy.
// All limits default to 0 = unlimited/inactive. Owner must configure
// per-channel via the PWA Settings page (future) or directly via NVS.
struct Channel {
  char name[MAX_NAME_BUF];
  Schedule sched[MAX_SCHEDULES];
  uint8_t schedCount;
  bool manualState;
  bool modeAuto;
  bool pirEnabled;
  uint16_t pirHoldTime;          // seconds, PIR hold time
  // Energy monitoring (software-estimated)
  uint32_t energyWh;             // accumulated watt-hours since last reset
  uint16_t wattage;              // user-configured wattage of the load (W)
  unsigned long lastOnMs;        // timestamp when relay last turned ON (for energy calc)

  // v4.2 safety limits (brief §13-16) — local-first, no network required
  uint32_t maxOnTimeSec;          // §14: 0=unlimited; >0=FORCE OFF after N seconds (e.g. 7200=2h)
  uint16_t minOnTimeSec;          // §15: 0=no min; >0=inhibit OFF before N seconds elapsed
  uint16_t minOffTimeSec;         // §15: 0=no min; >0=inhibit ON before N seconds elapsed
  uint16_t minSwitchIntervalSec;  // §15: 0=no limit; >0=min seconds between any two transitions
  uint8_t  bootPolicy;            // §13: 0=BOOT_OFF, 1=BOOT_ON, 2=RESTORE_LAST, 3=SAFE_STATE
  // Runtime tracking (not persisted — recomputed on boot)
  unsigned long lastTransitionMs;  // last ON↔OFF transition timestamp (anti-chatter)
  unsigned long onSinceMs;         // when relay currently ON, timestamp it turned ON (maxOnTime)
  bool maxOnTimeForced;            // true if maxOnTime caused FORCE OFF (alarm raised)
};

// ---------- BOOT POLICY ENUM (brief §13) ----------
enum class BootPolicy : uint8_t {
  BootOff      = 0,  // Default for hazardous loads
  BootOn       = 1,  // Only for loads that must auto-resume
  RestoreLast  = 2,  // Restore last known state from NVS
  SafeState    = 3,  // Channel-specific safe state (default OFF unless overridden)
};

// ---------- USER CONFIG (auth) ----------
struct UserConfig {
  char wwwUser[MAX_USER_BUF];
  char passHashHex[HASH_HEX_BUF_SIZE];
  uint8_t salt[SALT_LEN];
  uint16_t iterations;
};

// ---------- LOG TYPES ----------
enum class LogType : uint8_t {
  RelayOn = 0,
  RelayOff,
  PirTrigger,
  Login,
  Logout,
  Error,
  Restart,
  Ota,
  ConfigChange,
  FactoryReset,
  TimeSync,
  AuthFail,
};

// ---------- ACTIVITY LOG ENTRY ----------
struct ActivityLogEntry {
  uint32_t id;
  uint32_t timestamp;       // Unix epoch seconds
  LogType type;
  int8_t channelId;         // 0 = no channel, else 1..12
  char message[96];
};

// ---------- RELAY SOURCE ----------
enum class RelaySource : uint8_t {
  Off = 0,
  Manual,
  Schedule,
  Pir,
};

// ---------- AUTH ATTEMPT (rate limiting) ----------
struct AuthAttempt {
  uint32_t ip;              // packed IP
  uint8_t failCount;
  unsigned long lastFailTime;
  unsigned long blockUntil;
  bool active;
};

// ---------- PIR RUNTIME STATE ----------
struct PirState {
  bool motionNow;
  bool everTriggered;
  unsigned long lastMotion;
  unsigned long highSince;          // for stuck detection
  bool stuckAlerted;
  unsigned long stuckCooldownUntil;
  unsigned long lastSampleTime;
  uint8_t sampleHistory[3];         // PIR_DEBOUNCE_SAMPLES
  uint8_t sampleIdx;
  uint32_t triggerCountToday;
};

// ---------- SYSTEM METRICS ----------
// v4.2 audit (brief §44, §47): extended for Health Supervisor + crash forensics.
struct SystemMetrics {
  uint32_t bootTime;
  uint32_t lastDailyResetDay;
  uint32_t errorsToday;
  uint32_t pirTriggersToday[NUM_PIR];
  bool online;

  // v4.2 health/crash forensics (brief §44, §47)
  uint32_t bootCount;            // total boots (persisted in NVS)
  uint8_t  lastResetReason;      // esp_reset_reason_t (raw value)
  uint32_t watchdogResets;      // count of WDT-triggered resets
  uint32_t brownoutResets;      // count of brownout resets
  uint32_t wifiReconnectCount;
  uint32_t mqttReconnectCount;
  size_t   minFreeHeapObserved;  // lowest free heap seen since boot
  size_t   largestFreeBlock;     // largest free block right now
  uint32_t lastCrashUptime;     // uptime at last crash (0 if none)
  uint32_t telemetrySequence;    // §22: monotonic telemetry sequence counter
};

} // namespace Core

#endif // TIMER12_CORE_TYPES_H
