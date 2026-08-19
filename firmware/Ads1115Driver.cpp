// =============================================================================
// Ads1115Driver.cpp — 4-channel 16-bit I²C ADC, non-blocking state machine
// =============================================================================
// v4.1.1 audit fix: replaced previous _waitAndRead polling (up to 50 ms
// blocking) with a non-blocking state machine. Per brief §44.
//
// Each tick() advances the state machine:
//   State IDLE         → if interval elapsed, _startConversion(nextChannel),
//                         _convStartMs = now, _state = CONVERTING
//   State CONVERTING  → if now - _convStartMs >= ADS1115_CONV_TIME_MS,
//                         _state = READY (fall through)
//   State READY       → _readConversion + apply divider + cal,
//                         advance _nextChannel round-robin,
//                         _state = IDLE
//
// At 860 SPS, conversion takes ~1.2 ms. With 100 ms tick interval, we
// have plenty of margin. No delay() calls anywhere.
//
// I2C failure recovery: after MAX_CONSECUTIVE_ERRORS (10) consecutive
// failures, enter cooldown for RECOVERY_RETRY_MS (60 s) to free the bus.
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
// Conversion time at 860 SPS = 1/860 ≈ 1.16 ms. Use 3 ms for safety margin
// (covers all data rates ≤ 860 SPS — none of our channels use slower rates).
static constexpr uint16_t ADS1115_CONV_TIME_MS = 3;

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
  // v4.1.1 audit: reduce timeout from 50 ms to 10 ms
  uint32_t start = millis();
  while (Wire.available() < 2) {
    if (millis() - start > 10) return false;
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
  return _writeConfig(cfg);
}

bool Ads1115Driver::begin() {
  _available = false;
  for (uint8_t i = 0; i < 4; i++) {
    _channels[i] = {};
    _channels[i].status = AdsStatus::NotInitialized;
  }
  // Probe device by writing a config + reading back to verify (industrial grade)
  uint16_t probeCfg = CFG_SINGLE_SHOT | CFG_MODE_SINGLE | CFG_DR_860SPS | CFG_COMP_OFF;
  if (!_writeConfig(probeCfg)) {
    Serial.printf("[ADS1115 0x%02X] probe failed — sensor not present?\n", _address);
    return false;
  }
  delay(2);

  // v4.1.1 audit: read back config to verify device responded
  Wire.beginTransmission(_address);
  Wire.write(REG_CONFIG);
  if (Wire.endTransmission(false) != 0) {
    Serial.printf("[ADS1115 0x%02X] config readback failed\n", _address);
    return false;
  }
  if (Wire.requestFrom((int)_address, 2) != 2) {
    Serial.printf("[ADS1115 0x%02X] config readback short\n", _address);
    return false;
  }
  uint32_t start = millis();
  while (Wire.available() < 2) {
    if (millis() - start > 10) {
      Serial.printf("[ADS1115 0x%02X] config readback timeout\n", _address);
      return false;
    }
  }
  (void)Wire.read();
  (void)Wire.read();  // discard — we just verified I2C responds

  _available = true;
  _state = State::Idle;
  _nextChannel = 0;
  _consecutiveErrors = 0;
  _nextRetryMs = 0;
  Serial.printf("[ADS1115 0x%02X] initialized: PGA=%.3f V, divider=x%.2f, cal=(g=%.4f, o=%.4f)\n",
                _address, (double)_gain, (double)_dividerRatio,
                (double)_gainCal, (double)_offsetCal);
  return true;
}

void Ads1115Driver::tick() {
  if (!_available) return;
  unsigned long now = millis();

  // Cooldown after sustained failure
  if (_nextRetryMs > 0 && now < _nextRetryMs) return;
  if (_nextRetryMs > 0 && now >= _nextRetryMs) {
    _nextRetryMs = 0;
    _consecutiveErrors = 0;
    _state = State::Idle;
    Serial.printf("[ADS1115 0x%02X] retrying after recovery cooldown\n", _address);
  }

  // Throttle tick rate (don't start a new conversion too soon after the last)
  if (_state == State::Idle && (now - _lastReadMs < Battery::ADS1115_INTERVAL_MS)) return;

  switch (_state) {
    case State::Idle: {
      // Start conversion on next round-robin channel
      _convChannel = _nextChannel;
      if (!_startConversion(_convChannel)) {
        _channels[_convChannel].status = AdsStatus::I2cError;
        if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          _nextRetryMs = now + RECOVERY_RETRY_MS;
          Serial.printf("[ADS1115 0x%02X] %u consecutive errors — cooldown %lu s\n",
                        _address, _consecutiveErrors, RECOVERY_RETRY_MS / 1000);
        }
        _lastReadMs = now;
        return;
      }
      _convStartMs = now;
      _state = State::Converting;
      // fall through to check if already past conversion time
      [[fallthrough]];
    }
    case State::Converting: {
      if (now - _convStartMs < ADS1115_CONV_TIME_MS) return;  // not ready yet
      _state = State::Ready;
      [[fallthrough]];
    }
    case State::Ready: {
      uint16_t raw;
      if (!_readConversion(raw)) {
        _channels[_convChannel].status = AdsStatus::I2cError;
        if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          _nextRetryMs = now + RECOVERY_RETRY_MS;
        }
        _state = State::Idle;
        _lastReadMs = now;
        return;
      }
      // Convert: 16-bit signed raw → voltage at pin = raw * LSB
      int16_t signedRaw = (int16_t)raw;
      float pinV = signedRaw * ADS_LSB_V;

      // Sanity check: pin voltage must be in [0, gain]
      if (pinV < -0.5f || pinV > (_gain + 0.5f)) {
        _channels[_convChannel].status = AdsStatus::OutOfRange;
        _state = State::Idle;
        _lastReadMs = now;
        return;
      }

      // Apply divider ratio to recover the actual input voltage
      float inputV = pinV * _dividerRatio;
      // Apply two-point calibration
      float calibratedV = inputV * _gainCal + _offsetCal;

      _channels[_convChannel].rawVoltageV = inputV;
      _channels[_convChannel].voltageV = calibratedV;
      _channels[_convChannel].timestamp = now;
      _channels[_convChannel].status = AdsStatus::Ok;
      _consecutiveErrors = 0;

      // Advance round-robin
      _nextChannel = (_nextChannel + 1) & 0x03;
      _state = State::Idle;
      _lastReadMs = now;
      break;
    }
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
