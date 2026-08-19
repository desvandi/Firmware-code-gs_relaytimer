// =============================================================================
// BatteryVoltageDriver.h — Authoritative battery pack voltage source
// Timer Digital Relay v4.1
// -----------------------------------------------------------------------------
// Brief §10: "The authoritative pack voltage source must be Battery+ → Voltage
//   Divider → ESP32 ADC." Brief §10/§11 suggest GPIO34 for the ADC pin.
//
// GPIO AUDIT (see worklog Phase 0 + BatteryConfig.h deviation note):
//   GPIO34/35/36/39 are occupied by PIR 1..4. GPIO32/33 by I²C. GPIO37/38 are
//   NOT broken out on the standard WROOM-32 module. No ADC1 pin is available.
//   Brief §51/§52 forbid moving PIR pins without owner approval and require
//   STOP + REPORT rather than inventing a workaround.
//
// Resolution: PACK_VOLTAGE_SOURCE defaults to ADS1115_AIN3_BPLUS. Brief §16
// already routes B+ to ADS1115 #2's AIN3 channel (through the same divider as
// the other cell nodes). We reuse that calibrated reading as the authoritative
// pack voltage source. This avoids breaking PIR GPIO and reuses an existing
// signal path. The owner can switch to ESP32_ADC1 if they redesign PIR pins.
//
// Public contract:
//   getPackVoltageV()  → float V (or NaN if unavailable)
//   getSource()        → "ads1115_bplus" | "esp32_adc1" | "unavailable"
//   isAvailable()      → bool
// =============================================================================
#pragma once
#ifndef TIMER12_BATTERY_VOLTAGE_DRIVER_H
#define TIMER12_BATTERY_VOLTAGE_DRIVER_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"

namespace Drivers {

enum class PackVoltageStatus : uint8_t {
  Ok = 0,
  NotConfigured,
  I2cError,
  AdcInvalid,
  OutOfRange,
};

class BatteryVoltageDriver {
public:
  bool begin();
  void tick();   // 5–10 Hz raw sample, EMA-filtered output
  bool isAvailable() const { return _available; }
  float getPackVoltageV() const { return _filteredV; }
  PackVoltageStatus getStatus() const { return _status; }
  const char* getSourceStr() const;
  uint32_t getLastSampleMs() const { return _lastSampleMs; }

  // For ESP32_ADC1 source: returns the raw ADC count (0..4095) for diagnostics.
  uint16_t getLastRawAdc() const { return _lastRawAdc; }

private:
  bool         _available = false;
  PackVoltageStatus _status = PackVoltageStatus::NotConfigured;
  float        _filteredV = NAN;
  float        _emaV = 0.0f;
  bool         _emaInit = false;
  uint16_t     _lastRawAdc = 0;
  unsigned long _lastSampleMs = 0;

  bool     _sampleEsp32Adc();
  float    _readAds1115Bplus() const;
  uint16_t _oversampleAdc(uint8_t pin, uint16_t n) const;
};

extern BatteryVoltageDriver packVoltage;

} // namespace Drivers

#endif // TIMER12_BATTERY_VOLTAGE_DRIVER_H
