// =============================================================================
// Ads1115Driver.cpp — 4-channel 16-bit I²C ADC, raw I²C
// =============================================================================
// Each tick() starts ONE single-shot conversion on the next channel in the
// round-robin and reads the previous channel's result. With 860 SPS rate
// (1.16 ms per conversion) and 8 channels across 2 devices, a complete refresh
// takes ~8 ticks ≈ 800 ms at 100 ms tick interval. (Brief §44.)
//
// Calibration:  ADS reads raw mV at the input pin. Vinput = raw * divider.
//   vCalibrated = vRaw * gainCal + offsetCal
// (Brief §12: support voltageGain + voltageOffset.)
// =============================================================================
#include "Ads1115Driver.h"
#include "Config.h"
#include <Wire.h>
#include <cmath>

namespace Drivers {

Ads1115Driver adsCell1(Battery::ADS1115_CELL1_ADDR,
                       Battery::ADS_PGA_GAIN,
                       Battery::ADS_DIVIDER_RATIO,
                       Battery::ADS1_GAIN,
                       Battery::ADS1_OFFSET);
Ads1115Driver adsCell2(Battery::ADS1115_CELL2_ADDR,
                       Battery::ADS_PGA_GAIN,
                       Battery::ADS_DIVIDER_RATIO,
                       Battery::ADS2_GAIN,
                       Battery::ADS2_OFFSET);

// LSB for ±6.144 V FSR (gain 2/3) = 6.144 / 32768 = 187.5 µV
static constexpr float ADS_LSB_V = 0.0001875f;

Ads1115Driver::Ads1115Driver(uint8_t address, float gain, float dividerRatio,
                             float gainCal, float offsetCal)
  : _address(address & 0x7F),
    _gain(gain),
    _dividerRatio(dividerRatio > 0 ? dividerRatio : 1.0f),
    _gainCal(gainCal),
    _offsetCal(offsetCal) {}

bool Ads1115Driver::_writeConfig(uint16_t value) {
  Wire.beginTransmission(_address);
  Wire.write(REG_CONFIG);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Ads1115Driver::_readConversion(uint16_t& out) {
  Wire.beginTransmission(_address);
  Wire.write(REG_CONVERSION);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)_address, 2) != 2) return false;
  uint32_t start = millis();
  while (Wire.available() < 2) {
    if (millis() - start > 50) return false;
  }
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  out = ((uint16_t)hi << 8) | lo;
  return true;
}

bool Ads1115Driver::_startConversion(uint8_t channel) {
  // Build config: single-shot, channel vs GND, PGA ±6.144 V, 860 SPS, comp off
  // MUX bits for single-ended AIN-GND: 0b100 + channel (0..3)
  uint16_t mux = (uint16_t)(0x04 | (channel & 0x03)) << MUX_SHIFT;
  uint16_t cfg = CFG_SINGLE_SHOT | mux | CFG_MODE_SINGLE | CFG_DR_860SPS | CFG_COMP_OFF;
  // PGA bits = 000 for ±6.144 V (gain 2/3) — already 0 in our config
  return _writeConfig(cfg);
}

bool Ads1115Driver::_waitAndRead(uint8_t channel) {
  // Wait for conversion complete (bit15 of config = 1)
  uint32_t start = millis();
  uint16_t cfg;
  do {
    Wire.beginTransmission(_address);
    Wire.write(REG_CONFIG);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)_address, 2) != 2) return false;
    uint32_t wait = millis();
    while (Wire.available() < 2) {
      if (millis() - wait > 50) return false;
    }
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    cfg = ((uint16_t)hi << 8) | lo;
    if (millis() - start > 50) return false;  // 50 ms timeout
  } while ((cfg & 0x8000) == 0);

  uint16_t raw;
  if (!_readConversion(raw)) return false;

  // Convert: 16-bit signed raw → voltage at pin = raw * LSB
  int16_t signedRaw = (int16_t)raw;
  float pinV = signedRaw * ADS_LSB_V;

  // Sanity check: pin voltage must be in [0, gain]
  if (pinV < -0.5f || pinV > (_gain + 0.5f)) {
    _channels[channel].status = AdsStatus::OutOfRange;
    return false;
  }

  // Apply divider ratio to recover the actual input voltage
  float inputV = pinV * _dividerRatio;

  // Apply two-point calibration
  float calibratedV = inputV * _gainCal + _offsetCal;

  _channels[channel].rawVoltageV = inputV;
  _channels[channel].voltageV = calibratedV;
  _channels[channel].timestamp = millis();
  _channels[channel].status = AdsStatus::Ok;
  return true;
}

bool Ads1115Driver::begin() {
  _available = false;
  for (uint8_t i = 0; i < 4; i++) {
    _channels[i] = {};
    _channels[i].status = AdsStatus::NotInitialized;
  }
  // Probe device by writing a config
  if (!_writeConfig(CFG_SINGLE_SHOT | CFG_MODE_SINGLE | CFG_DR_860SPS | CFG_COMP_OFF)) {
    Serial.printf("[ADS1115 0x%02X] probe failed — sensor not present?\n", _address);
    return false;
  }
  delay(2);
  _available = true;
  Serial.printf("[ADS1115 0x%02X] initialized: PGA=±%.3f V, divider=×%.2f, cal=(g=%.4f, o=%.4f)\n",
                _address, (double)_gain, (double)_dividerRatio,
                (double)_gainCal, (double)_offsetCal);
  // Kick off first conversion
  _startConversion(0);
  _nextChannel = 0;
  return true;
}

void Ads1115Driver::tick() {
  if (!_available) return;
  unsigned long now = millis();
  if (now - _lastReadMs < Battery::ADS1115_INTERVAL_MS) return;
  _lastReadMs = now;

  // Read previous conversion result, then start next.
  uint8_t chToRead = _nextChannel;
  if (!_waitAndRead(chToRead)) {
    _channels[chToRead].status = AdsStatus::I2cError;
    // Mark stale (do NOT overwrite last good value — just flag status)
  }
  // Advance round-robin
  _nextChannel = (_nextChannel + 1) & 0x03;
  if (!_startConversion(_nextChannel)) {
    _channels[_nextChannel].status = AdsStatus::I2cError;
  }
}

float Ads1115Driver::getChannelV(uint8_t ch) const {
  if (ch > 3) return NAN;
  if (_channels[ch].status != AdsStatus::Ok) return NAN;
  return _channels[ch].voltageV;
}

AdsStatus Ads1115Driver::getChannelStatus(uint8_t ch) const {
  if (ch > 3) return AdsStatus::OutOfRange;
  return _channels[ch].status;
}

uint32_t Ads1115Driver::getChannelTime(uint8_t ch) const {
  if (ch > 3) return 0;
  return _channels[ch].timestamp;
}

} // namespace Drivers
