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
struct SystemMetrics {
  uint32_t bootTime;
  uint32_t lastDailyResetDay;
  uint32_t errorsToday;
  uint32_t pirTriggersToday[NUM_PIR];
  bool online;
};

} // namespace Core

#endif // TIMER12_CORE_TYPES_H
