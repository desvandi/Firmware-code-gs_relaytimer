// =============================================================================
// Ads1115Driver.h — 4-channel 16-bit I²C ADC for cumulative cell-node measurement
// Timer Digital Relay v4.1 — Battery Cell Monitoring
// -----------------------------------------------------------------------------
// Two instances per brief §13/§16:
//   adsCell1  (addr 0x48)  AIN0=C1, AIN1=C2, AIN2=C3, AIN3=C4
//   adsCell2  (addr 0x49)  AIN0=C5, AIN1=C6, AIN2=C7, AIN3=B+
//
// Each AIN input receives a DIVIDED cell-node voltage (ADS_DIVIDER_RATIO = 11)
// so that 29.2 V at B+ maps to 2.65 V at the ADS1115 input — well inside the
// ±6.144 V PGA full-scale range (gain 2/3).
//
// The driver NEVER exposes cumulative node voltages as if they were cell
// voltages. Cell voltages are computed by BatteryMonitor from successive
// differences (brief §14). Public contract: cellVoltage[8] only.
//
// Non-blocking: tick() reads one channel per call (round-robin) so no single
// I2C transaction blocks the main loop. (Brief §44.)
// =============================================================================
#pragma once
#ifndef TIMER12_ADS1115_DRIVER_H
#define TIMER12_ADS1115_DRIVER_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"

namespace Drivers {

enum class AdsStatus : uint8_t {
  Ok = 0,
  NotInitialized,
  I2cError,
  ConversionTimeout,
  OutOfRange,
};

// Single-channel reading
struct AdsReading {
  float    voltageV;        // calibrated V at the AIN pin
  float    rawVoltageV;     // raw V before gain/offset calibration
  uint32_t timestamp;
  AdsStatus status;
};

class Ads1115Driver {
public:
  // address: I2C 7-bit address (0x48..0x4B)
  // gain: PGA full-scale V (typically 6.144 for ±6.144 V)
  // dividerRatio: external resistor-divider ratio (Vinput / Vpin)
  // gainCal/offsetCal: two-point calibration (brief §12)
  Ads1115Driver(uint8_t address, float gain, float dividerRatio,
                float gainCal = 1.0f, float offsetCal = 0.0f);

  bool begin();
  void tick();   // reads one channel per call (round-robin) → ~8 ticks to refresh all
  bool isAvailable() const { return _available; }

  // Returns the calibrated voltage for channel ch (0..3). If never read or
  // invalid, returns NaN.
  float getChannelV(uint8_t ch) const;
  AdsStatus getChannelStatus(uint8_t ch) const;
  uint32_t getChannelTime(uint8_t ch) const;

private:
  uint8_t      _address;
  float        _gain;            // PGA full-scale V (e.g. 6.144)
  float        _dividerRatio;    // external divider ratio
  float        _gainCal;
  float        _offsetCal;
  bool         _available = false;
  AdsReading   _channels[4] = {};
  uint8_t      _nextChannel = 0;
  unsigned long _lastReadMs = 0;

  // ADS1115 register addresses
  enum Reg : uint8_t {
    REG_CONVERSION = 0x00,
    REG_CONFIG     = 0x01,
  };

  // Config bits (ADS1115 datasheet §9.6.3)
  // bit15:  1 = begin single conversion (operational single-shot mode)
  // bits14-12: MUX (000=A0-A1 diff, 100=A0-GND, 101=A1-GND, 110=A2-GND, 111=A3-GND)
  // bits11-9:  PGA (011 = ±1.024 V, 100 = ±2.048 V, 010 = ±4.096 V, 001 = ±4.096 V alt,
  //                   000 = ±6.144 V (gain 2/3))
  // bit8:   0 = continuous conversion, 1 = single-shot
  // bits7-5: data rate (100 = 128 SPS, 101 = 250 SPS, 110 = 475 SPS, 111 = 860 SPS)
  // bit4:   0 = traditional comparator (unused)
  // bit3:   0 = comparator low (unused)
  // bit2:   0 = non-latching (unused)
  // bit1:   0 = comparator assert low (unused)
  // bit0:   0 = disable comparator (unused)
  static constexpr uint16_t MUX_SHIFT = 12;
  static constexpr uint16_t PGA_MASK  = 0x0E00;
  static constexpr uint16_t CFG_SINGLE_SHOT = 0x8000;
  static constexpr uint16_t CFG_MODE_SINGLE  = 0x0100;
  static constexpr uint16_t CFG_DR_860SPS    = 0x00E0;
  static constexpr uint16_t CFG_COMP_OFF     = 0x0003;

  bool _writeConfig(uint16_t value);
  bool _readConversion(uint16_t& out);
  bool _startConversion(uint8_t channel);
  bool _waitAndRead(uint8_t channel);
};

// Pre-defined instances (brief §13, §16)
extern Ads1115Driver adsCell1;
extern Ads1115Driver adsCell2;

} // namespace Drivers

#endif // TIMER12_ADS1115_DRIVER_H
