// =============================================================================
// HealthSupervisor.h — System health monitoring (brief §44)
// Timer Digital Relay v4.2 — Industrial-Grade Hardening
// -----------------------------------------------------------------------------
// Monitors:
//   uptime, bootCount, resetReason, watchdogCount, brownoutCount, freeHeap,
//   minimumFreeHeap, largestFreeBlock, task stack, wifiRSSI,
//   wifiReconnectCount, mqttReconnectCount, rtcStatus, pzemStatus,
//   pirStatus, filesystemStatus, nvsStatus, firmwareVersion, otaState
//
// Also implements RTOS task heartbeat monitoring (brief §45):
//   RelayEngine / MQTT / Telemetry / Scheduler / PIR / PZEM / OTA /
//   HealthMonitor — each task heartbeats via recordHeartbeat(taskId).
//   If heartbeat timeout exceeds threshold → TASK_STALL alarm raised.
// =============================================================================
#pragma once
#ifndef TIMER12_HEALTH_SUPERVISOR_H
#define TIMER12_HEALTH_SUPERVISOR_H

#include <Arduino.h>
#include <cstdint>
#include "AlarmRegistry.h"
#include "ErrorCodes.h"

namespace Services {

enum class RtcStatus : uint8_t {
  Valid    = 0,  // §18: RTC is initialized and time is plausible
  Invalid  = 1,  // §18: RTC present but time out of range (battery dead?)
  Unsynced = 2,  // §18: RTC not initialized yet
};

inline const char* rtcStatusStr(RtcStatus s) {
  switch (s) {
    case RtcStatus::Valid:    return "VALID";
    case RtcStatus::Invalid:  return "INVALID";
    case RtcStatus::Unsynced: return "UNSYNCED";
  }
  return "UNSYNCED";
}

enum class SensorStatus : uint8_t {
  Valid        = 0,  // §20: sensor reading is current and within plausible range
  Stale        = 1,  // §20: last successful read too old
  Error        = 2,  // §20: I2C/CRC/communication error
  Unavailable  = 3,  // §20: sensor not detected at boot / not configured
};

inline const char* sensorStatusStr(SensorStatus s) {
  switch (s) {
    case SensorStatus::Valid:       return "VALID";
    case SensorStatus::Stale:       return "STALE";
    case SensorStatus::Error:        return "ERROR";
    case SensorStatus::Unavailable: return "UNAVAILABLE";
  }
  return "UNAVAILABLE";
}

enum class TaskId : uint8_t {
  RelayEngine = 0,
  Mqtt        = 1,
  Telemetry   = 2,
  Scheduler   = 3,
  Pir         = 4,
  Pzem        = 5,
  Ota         = 6,
  HealthMonitor = 7,
  BatteryMonitor = 8,
  COUNT
};
constexpr uint8_t TASK_COUNT = (uint8_t)TaskId::COUNT;

// v4.3 audit P1-010: Health state machine per ChatGPT audit:
//   "Tambahkan state: HEALTHY, WARNING, DEGRADED, FAILED, RECOVERING
//    dan recovery policy per subsystem."
enum class HealthState : uint8_t {
  Healthy     = 0,
  Warning     = 1,
  Degraded    = 2,  // one or more subsystems in fault, system operational
  Failed       = 3,  // critical subsystem failed — system in safe state
  Recovering  = 4,  // recovery in progress (e.g., OTA rollback, MQTT reconnect)
};

inline const char* healthStateStr(HealthState s) {
  switch (s) {
    case HealthState::Healthy:    return "HEALTHY";
    case HealthState::Warning:     return "WARNING";
    case HealthState::Degraded:    return "DEGRADED";
    case HealthState::Failed:       return "FAILED";
    case HealthState::Recovering: return "RECOVERING";
  }
  return "UNKNOWN";
}

struct HealthSnapshot {
  uint32_t uptimeSeconds;
  uint32_t bootCount;
  uint8_t  lastResetReason;
  uint32_t watchdogResets;
  uint32_t brownoutResets;
  size_t   freeHeap;
  size_t   minFreeHeap;
  size_t   largestFreeBlock;
  int8_t   wifiRssi;
  uint32_t wifiReconnectCount;
  uint32_t mqttReconnectCount;
  RtcStatus rtcStatus;
  SensorStatus pzemStatus;
  SensorStatus pirStatus;
  SensorStatus sht31Status;
  SensorStatus ina219Status;
  SensorStatus ads1115Status;
  bool     filesystemOk;
  bool     nvsOk;
  bool     mqttConnected;
  uint32_t telemetrySequence;
  // Per-task heartbeat ages (ms since last heartbeat)
  uint32_t taskHeartbeatAgeMs[TASK_COUNT];
  AlarmSeverity highestAlarm;
  // v4.3 P1-010: aggregated health state
  HealthState systemState;
  // v4.3 P1-011: boot loop detection (time-window based)
  bool     bootLoopDetected;
  uint8_t  bootsInLast60s;
};

class HealthSupervisor {
public:
  void begin();
  void tick();

  // Record events
  void recordBoot();
  void recordWifiReconnect();
  void recordMqttReconnect();
  void recordHeartbeat(TaskId id);
  void recordCrash(uint32_t uptimeAtCrash);

  // RTC status — set by RtcDriver or boot logic
  void setRtcStatus(RtcStatus s);
  RtcStatus getRtcStatus() const { return _rtcStatus; }

  // Sensor statuses — set by respective drivers
  void setPzemStatus(SensorStatus s) { _pzemStatus = s; }
  void setPirStatus(SensorStatus s) { _pirStatus = s; }
  void setSht31Status(SensorStatus s) { _sht31Status = s; }
  void setIna219Status(SensorStatus s) { _ina219Status = s; }
  void setAds1115Status(SensorStatus s) { _adsStatus = s; }

  HealthSnapshot getSnapshot() const { return _snapshot; }
  uint32_t nextTelemetrySequence();  // atomically increment + return

  // Reset reason classification (matches esp_reset_reason_t values)
  static const char* resetReasonStr(uint8_t reason);

  // v4.3 P1-010: Get aggregated health state
  HealthState getSystemState() const { return _snapshot.systemState; }

private:
  HealthSnapshot _snapshot = {};
  RtcStatus  _rtcStatus = RtcStatus::Unsynced;
  SensorStatus _pzemStatus = SensorStatus::Unavailable;
  SensorStatus _pirStatus = SensorStatus::Unavailable;
  SensorStatus _sht31Status = SensorStatus::Unavailable;
  SensorStatus _ina219Status = SensorStatus::Unavailable;
  SensorStatus _adsStatus = SensorStatus::Unavailable;
  unsigned long _lastHeartbeatMs[TASK_COUNT] = {};
  unsigned long _lastTickMs = 0;
  bool _initialized = false;

  // v4.3 P1-011: boot timestamp ring buffer for time-window boot loop detection
  static constexpr uint8_t BOOT_LOOP_WINDOW = 8;
  uint32_t _bootTimestamps[BOOT_LOOP_WINDOW] = {};
  uint8_t  _bootTimestampIdx = 0;

  // v4.3 P1-010: Compute aggregated health state from snapshot
  void _recomputeSystemState();
};

extern HealthSupervisor health;

} // namespace Services

#endif // TIMER12_HEALTH_SUPERVISOR_H
