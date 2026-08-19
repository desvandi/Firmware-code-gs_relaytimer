// =============================================================================
// Ina219Driver.h — INA219 bidirectional current sensor (raw I²C, no lib dep)
// Timer Digital Relay v4.1 — DC Energy Monitoring
// -----------------------------------------------------------------------------
// Two instances:
//   ina219Battery  (addr 0x40)  — battery branch current (bidirectional)
//   ina219Inverter (addr 0x41)  — inverter branch current (DC consumption)
//
// External shunt: 0.75 mΩ (75 mV @ 100 A). Onboard R100 removed. (Brief §8.)
// Polarity corrected in software per Brief §7 — INA219 reports signed shunt
// voltage; we apply BATTERY_CURRENT_SIGN / INVERTER_CURRENT_SIGN so that the
// public contract matches Brief §5/§6:
//   Ibattery > 0 = discharge,  Ibattery < 0 = charge
//   Iinverter > 0 = consumption
//
// Calibration register computed for the external shunt so that the raw
// current register returns mA directly (see _computeCalibration).
//
// Sensor failure → isAvailable()=false; system continues (brief §46).
// =============================================================================
#pragma once
#ifndef TIMER12_INA219_DRIVER_H
#define TIMER12_INA219_DRIVER_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"

namespace Drivers {

enum class Ina219Status : uint8_t {
  Ok = 0,
  NotInitialized,
  I2cError,
  OutOfRange,
  Stale,
};

struct Ina219Reading {
  float    shuntVoltageV;   // raw V across shunt (signed)
  float    busVoltageV;     // V at INA219 VBUS pin (NOT authoritative pack V)
  float    currentA;         // signed, post-polarity-correction
  float    powerW;           // busV * currentA (signed)
  uint32_t timestamp;       // millis() of last successful read
  Ina219Status status;
};

class Ina219Driver {
public:
  // address: I2C 7-bit address (0x40..0x4F)
  // shuntOhms: external shunt resistance (e.g. 0.00075 for 0.75 mΩ)
  // signCorrection: +1.0 or -1.0 to align with semantic contract
  Ina219Driver(uint8_t address, float shuntOhms, float signCorrection);

  bool begin();                 // call from setup() after Wire.begin()
  void tick();                  // call from loop() — non-blocking
  bool isAvailable() const { return _available; }
  Ina219Reading getReading() const { return _reading; }

  // Convenience accessors
  float getCurrent() const { return _reading.currentA; }
  float getBusVoltage() const { return _reading.busVoltageV; }
  float getShuntVoltage() const { return _reading.shuntVoltageV; }
  uint32_t getLastReadMs() const { return _reading.timestamp; }

private:
  uint8_t  _address;
  float    _shuntOhms;
  float    _signCorrection;
  bool     _available = false;
  Ina219Reading _reading = {};
  unsigned long _lastReadMs = 0;
  float    _emaCurrent = 0.0f;   // EMA smoothing (brief §45)
  bool     _emaInit = false;

  // v4.1.1 audit: I2C failure recovery — after MAX consecutive errors,
  // enter cooldown for RECOVERY_RETRY_MS (60 s) to free I2C bus bandwidth
  // for other sensors (brief §46).
  uint8_t  _consecutiveErrors = 0;
  unsigned long _nextRetryMs = 0;
  static constexpr uint16_t MAX_CONSECUTIVE_ERRORS = 10;
  static constexpr uint32_t RECOVERY_RETRY_MS = 60000;

  // Register addresses (INA219 datasheet)
  enum Reg : uint8_t {
    REG_CONFIG     = 0x00,
    REG_SHUNT      = 0x01,
    REG_BUS        = 0x02,
    REG_POWER      = 0x03,
    REG_CURRENT    = 0x04,
    REG_CALIBRATION = 0x05,
  };

  // Config: 32 V FSR, PGA ±320 mV (shunt max 320 mV → 100 A @ 0.75 mΩ = 75 mV, headroom)
  // 16 samples averaging for shunt + bus → ~68 ms conversion per channel.
  static constexpr uint16_t CONFIG_RESET = 0x399F;

  bool _writeRegister(uint8_t reg, uint16_t value);
  bool _readRegister(uint8_t reg, uint16_t& out);
  uint16_t _computeCalibration() const;
  int16_t  _signExtend16(uint16_t raw) const;
};

// Two pre-defined instances (brief §3, §15)
extern Ina219Driver ina219Battery;
extern Ina219Driver ina219Inverter;

} // namespace Drivers

#endif // TIMER12_INA219_DRIVER_H
