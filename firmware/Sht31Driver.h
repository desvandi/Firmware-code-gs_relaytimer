// =============================================================================
// Sht31Driver.h — SHT31 ambient T/RH sensor (I²C, 0x44 or 0x45)
// Timer Digital Relay v4.1 — Environmental Monitoring
// -----------------------------------------------------------------------------
// Exposes ambientTemperature + ambientHumidity. (Brief §20.)
// IMPORTANT: do NOT label SHT31 temperature as battery temperature —
//   it measures environmental/ambient conditions.
// Sensor failure → isAvailable()=false; system continues. (Brief §46.)
// Stale-data detection: if no successful read for > STALE_MS, mark invalid.
// =============================================================================
#pragma once
#ifndef TIMER12_SHT31_DRIVER_H
#define TIMER12_SHT31_DRIVER_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"

namespace Drivers {

enum class Sht31Status : uint8_t {
  Ok = 0,
  NotInitialized,
  I2cError,
  CrcError,
  Stale,
  OutOfRange,
};

struct Sht31Reading {
  float    temperatureC;
  float    humidityRH;
  uint32_t timestamp;
  Sht31Status status;
};

class Sht31Driver {
public:
  explicit Sht31Driver(uint8_t address = Battery::SHT31_ADDR);

  bool begin();
  void tick();
  bool isAvailable() const { return _available; }
  Sht31Reading getReading() const { return _reading; }
  float getTemperatureC() const { return _reading.temperatureC; }
  float getHumidityRH() const { return _reading.humidityRH; }

private:
  uint8_t      _address;
  bool         _available = false;
  Sht31Reading _reading = {};
  unsigned long _lastReadMs = 0;

  // v4.1.1 audit: non-blocking state machine (replaces previous delay(15)
  // which violated brief §44 "no sensor acquisition may block relay/MQTT/
  // REST/OTA/scheduler/watchdog/transaction processing").
  //   State IDLE          → if interval elapsed, send measure cmd → State CONVERTING
  //   State CONVERTING    → wait for SHT31_CONV_TIME_MS (no delay!) → State READY
  //   State READY         → read 6 bytes + CRC, update reading → State IDLE
  enum class State : uint8_t {
    Idle = 0,
    Converting,
    Ready,
  };
  State    _state = State::Idle;
  unsigned long _stateEnteredMs = 0;

  // I2C failure recovery (brief §46 — no infinite retry, no spam)
  uint8_t  _consecutiveErrors = 0;
  unsigned long _nextRetryMs = 0;  // when unavailable, only retry after this time
  static constexpr uint16_t MAX_CONSECUTIVE_ERRORS = 10;
  static constexpr uint32_t RECOVERY_RETRY_MS = 60000;  // 60 s slow retry after sustained failure

  bool _sendCommand(uint16_t cmd);
  bool _read6(uint8_t out[6]);
  bool _crc8(const uint8_t* data, uint8_t len, uint8_t expected) const;
};

extern Sht31Driver sht31;

} // namespace Drivers

#endif // TIMER12_SHT31_DRIVER_H
