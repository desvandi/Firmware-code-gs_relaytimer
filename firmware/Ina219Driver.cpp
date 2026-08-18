// =============================================================================
// Ina219Driver.cpp — bidirectional current sensor (raw I²C)
// =============================================================================
// Calibration derivation (INA219 datasheet §6, "Programming the Calibration
// register"):
//
//   calValue = trunc(0.04096 / (current_LSB * Rshunt))
//
// where current_LSB (A/bit) is chosen so that the expected max current fits
// in 15 bits (signed). For 100 A max with a 0.75 mΩ shunt, we pick
// current_LSB = 4 mA/bit (so 100 A = 25 000 counts → fits).
//
//   calValue = trunc(0.04096 / (0.004 * 0.00075)) = 13653 ≈ 0x3555
//
// Power register (W) = current_LSB × power_LSB_factor × 20 = current * busV
// (per datasheet). The driver recomputes power from busV*current to ensure
// correct sign semantics for our polarity-corrected current.
//
// All I2C failures mark isAvailable()=false — no blocking retry. (Brief §46.)
// =============================================================================
#include "Ina219Driver.h"
#include "Config.h"  // Core::I2C_SDA / SCL (already initialized by DS3231)
#include <Wire.h>
#include <cmath>

namespace Drivers {

// Pre-defined instances (brief §3, §15)
Ina219Driver ina219Battery(Battery::INA219_BATTERY_ADDR,
                           Battery::BATTERY_SHUNT_OHMS,
                           Battery::BATTERY_CURRENT_SIGN);
Ina219Driver ina219Inverter(Battery::INA219_INVERTER_ADDR,
                            Battery::INVERTER_SHUNT_OHMS,
                            Battery::INVERTER_CURRENT_SIGN);

// 4 mA/bit LSB → 100 A / 4 mA = 25000 counts (fits int16 signed max ±32767)
static constexpr float CURRENT_LSB_A = 0.004f;

Ina219Driver::Ina219Driver(uint8_t address, float shuntOhms, float signCorrection)
  : _address(address & 0x7F), _shuntOhms(shuntOhms), _signCorrection(signCorrection) {}

uint16_t Ina219Driver::_computeCalibration() const {
  // calValue = 0.04096 / (current_LSB * Rshunt)
  // Guard against divide-by-zero / negative shunt.
  if (_shuntOhms <= 0.0f) return 0;
  float cal = 0.04096f / (CURRENT_LSB_A * _shuntOhms);
  if (cal < 1.0f || cal > 65535.0f) return 0;
  return (uint16_t)cal;
}

bool Ina219Driver::_writeRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(_address);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Ina219Driver::_readRegister(uint8_t reg, uint16_t& out) {
  Wire.beginTransmission(_address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // restart
  if (Wire.requestFrom((int)_address, 2) != 2) return false;
  uint32_t start = millis();
  while (Wire.available() < 2) {
    if (millis() - start > 50) return false;  // 50 ms timeout (non-blocking-ish)
  }
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  out = ((uint16_t)hi << 8) | lo;
  return true;
}

int16_t Ina219Driver::_signExtend16(uint16_t raw) const {
  return (int16_t)raw;  // C++ implicit sign extension for int16_t
}

bool Ina219Driver::begin() {
  _available = false;
  _reading = {};
  _reading.status = Ina219Status::NotInitialized;

  // Reset device first
  if (!_writeRegister(REG_CONFIG, CONFIG_RESET)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] reset failed — sensor not present?\n", _address);
    return false;
  }
  delay(2);  // INA219 datasheet: 100 µs minimum after reset

  // Configure: 32 V FSR (bit13=0), shunt PGA ±320 mV (bits11-12=00),
  // 16-sample averaging for both shunt and bus (bits7-10=1111, bits3-6=1111),
  // shunt + bus continuous (bits0-2=11).
  // Binary: 0000 1111 1000 1111 1111 0011 = 0x3F8F + (10<<3) wait recheck
  // Actual: CONFIG = 32V/320mV/12-bit/16-sample/16-sample/shunt+bus-continuous
  //   bits15-13: 000 (32 V FSR)
  //   bits12-11: 00 (PGA ±320 mV)
  //   bits10-7:  1111 (shunt 16-sample avg)  → 17.6 ms conversion
  //   bits6-3:   1111 (bus 16-sample avg)
  //   bits2-0:   011 (shunt+bus continuous)
  uint16_t config = 0b0000000011111111; // 0x00FF
  config |= (0b1111 << 7);              // shunt avg
  config |= (0b1111 << 3);              // bus avg
  config |= 0b011;                       // continuous shunt+bus
  // → 0x3FFF (32V, 320 mV PGA, 16-sample both, continuous)
  if (!_writeRegister(REG_CONFIG, config)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] config write failed\n", _address);
    return false;
  }

  // Write calibration register
  uint16_t cal = _computeCalibration();
  if (cal == 0) {
    Serial.printf("[INA219 0x%02X] calibration computation invalid (shunt=%f)\n",
                  _address, (double)_shuntOhms);
    return false;
  }
  if (!_writeRegister(REG_CALIBRATION, cal)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] calibration write failed\n", _address);
    return false;
  }

  _available = true;
  _reading.status = Ina219Status::Ok;
  Serial.printf("[INA219 0x%02X] initialized: shunt=%.4f mΩ, cal=0x%04X, sign=%+.1f\n",
                _address, (double)(_shuntOhms * 1000.0f), cal, _signCorrection);
  return true;
}

void Ina219Driver::tick() {
  if (!_available) return;
  unsigned long now = millis();
  if (now - _lastReadMs < Battery::INA219_INTERVAL_MS) return;
  _lastReadMs = now;

  uint16_t shuntRaw, busRaw;
  if (!_readRegister(REG_SHUNT, shuntRaw) || !_readRegister(REG_BUS, busRaw)) {
    _reading.status = Ina219Status::I2cError;
    return;
  }

  // Shunt voltage: 16-bit signed, LSB = 10 µV (brief datasheet)
  int16_t shuntSigned = _signExtend16(shuntRaw);
  float shuntV = shuntSigned * 0.00001f;  // 10 µV/bit

  // Bus voltage: bits15-3 of register, LSB = 4 mV, bits2-0 are status bits
  uint16_t busFixed = (busRaw >> 3) & 0x1FFF;  // mask 13-bit value
  float busV = busFixed * 0.004f;  // 4 mV/bit
  if (busFixed == 0x1FFF) busV = 0;  // overflow → treat as invalid
  // Note: INA219 bus voltage is NOT authoritative pack V (brief §3) —
  //   8S LiFePO4 reaches 29.2 V, exceeding the INA219 26 V practical range.

  // Compute current (A) from shunt voltage and external shunt resistance.
  // (current_A = shunt_V / Rshunt). Apply polarity correction per brief §7.
  float rawCurrent = (shuntV / _shuntOhms);   // A
  float correctedCurrent = rawCurrent * _signCorrection;

  // Reject impossible spikes (brief §23)
  if (std::isinf(correctedCurrent) || std::isnan(correctedCurrent) ||
      std::fabs(correctedCurrent) > Battery::CURRENT_SPIKE_REJECT_A) {
    _reading.status = Ina219Status::OutOfRange;
    return;
  }

  // EMA smoothing (brief §45) — separate filter so raw step not over-smoothed
  if (!_emaInit) {
    _emaCurrent = correctedCurrent;
    _emaInit = true;
  } else {
    _emaCurrent = _emaCurrent * (1.0f - Battery::CURRENT_SMOOTH_ALPHA)
                  + correctedCurrent * Battery::CURRENT_SMOOTH_ALPHA;
  }

  _reading.shuntVoltageV = shuntV;
  _reading.busVoltageV = busV;
  _reading.currentA = _emaCurrent;
  _reading.powerW = busV * _emaCurrent;  // signed power
  _reading.timestamp = now;
  _reading.status = Ina219Status::Ok;
}

} // namespace Drivers
