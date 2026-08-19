// =============================================================================
// Sht31Driver.cpp — SHT31 ambient T/RH (raw I²C, non-blocking state machine)
// =============================================================================
// v4.1.1 audit fix: replaced previous delay(15) blocking call with a
// non-blocking state machine. Per brief §44: "No sensor acquisition may
// block relay control, MQTT, REST, OTA, scheduler, watchdog, or
// transaction processing."
//
// State machine:
//   State IDLE          → if interval elapsed and not in cooldown,
//                          send measure cmd (0x2C06, no clock stretch),
//                          transition to CONVERTING.
//   State CONVERTING   → wait SHT31_CONV_TIME_MS (15 ms typical, 20 ms safe)
//                          WITHOUT calling delay(). Transition to READY.
//   State READY        → read 6 bytes [T_msb, T_lsb, T_crc,
//                          RH_msb, RH_lsb, RH_crc] + verify CRC-8.
//                          Update reading, transition to IDLE.
//
// I2C failure recovery:
//   On any I2C/CRC error, increment _consecutiveErrors. After
//   MAX_CONSECUTIVE_ERRORS (10) consecutive failures, mark sensor
//   unavailable and only retry after RECOVERY_RETRY_MS (60 s) — frees
//   I2C bus bandwidth for other sensors and avoids log spam.
//
// SHT31 single-shot command 0x2C06 (high repeatability, clock stretch
// disabled — we use timed wait instead). Conversion:
//   T_C = -45 + 175 * (rawT / 65535)
//   RH  = 100 * (rawRH / 65535)   (clamp 0..100)
//
// CRC-8: poly=0x31, init=0xFF
// =============================================================================
// BATTERY_MONITORING_ENABLED guard — file compiles to nothing when battery
// monitoring is disabled (saves flash for relay-only installations).
#include "BatteryConfig.h"
#if !BATTERY_ENABLED
// File intentionally empty when battery monitoring is disabled.
#else
#include "Sht31Driver.h"
#include "Config.h"
#include <Wire.h>
#include <cmath>

namespace Drivers {

Sht31Driver sht31(Battery::SHT31_ADDR);

static constexpr uint32_t SHT31_STALE_MS = 5000;     // 5 s without read = stale
static constexpr uint16_t SHT31_CONV_TIME_MS = 20;  // 20 ms safe margin (datasheet: 15 ms max high-rep)

Sht31Driver::Sht31Driver(uint8_t address) : _address(address & 0x7F) {}

bool Sht31Driver::_sendCommand(uint16_t cmd) {
  Wire.beginTransmission(_address);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Sht31Driver::_read6(uint8_t out[6]) {
  if (Wire.requestFrom((int)_address, 6) != 6) return false;
  // v4.1.1 audit: reduce timeout from 100 ms to 10 ms — non-blocking
  uint32_t start = millis();
  while (Wire.available() < 6) {
    if (millis() - start > 10) return false;
  }
  for (uint8_t i = 0; i < 6; i++) out[i] = Wire.read();
  return true;
}

bool Sht31Driver::_crc8(const uint8_t* data, uint8_t len, uint8_t expected) const {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x80) crc = (crc << 1) ^ 0x31;
      else            crc = (crc << 1);
    }
  }
  return crc == expected;
}

bool Sht31Driver::begin() {
  _available = false;
  _reading = {};
  _reading.status = Sht31Status::NotInitialized;
  _state = State::Idle;
  _stateEnteredMs = 0;
  _consecutiveErrors = 0;
  _nextRetryMs = 0;

  // Soft-reset (command 0x30A2) then wait 2 ms (one-time init — acceptable blocking)
  if (!_sendCommand(0x30A2)) {
    Serial.printf("[SHT31 0x%02X] not present (soft-reset failed)\n", _address);
    return false;
  }
  delay(2);

  // Try one conversion to verify
  if (!_sendCommand(0x2C06)) {
    _reading.status = Sht31Status::I2cError;
    Serial.printf("[SHT31 0x%02X] measure command failed\n", _address);
    return false;
  }
  delay(SHT31_CONV_TIME_MS);  // acceptable at init
  uint8_t buf[6];
  if (!_read6(buf)) {
    _reading.status = Sht31Status::I2cError;
    Serial.printf("[SHT31 0x%02X] read failed\n", _address);
    return false;
  }
  if (!_crc8(buf, 2, buf[2]) || !_crc8(buf + 3, 2, buf[5])) {
    _reading.status = Sht31Status::CrcError;
    Serial.printf("[SHT31 0x%02X] CRC mismatch\n", _address);
    return false;
  }
  _available = true;
  _reading.status = Sht31Status::Ok;
  Serial.printf("[SHT31 0x%02X] initialized (non-blocking state machine)\n", _address);
  return true;
}

void Sht31Driver::tick() {
  if (!_available) return;
  unsigned long now = millis();

  // Cooldown after sustained failure — skip entirely until retry window
  if (_nextRetryMs > 0 && now < _nextRetryMs) return;
  if (_nextRetryMs > 0 && now >= _nextRetryMs) {
    // Time to retry — attempt re-init in background by transitioning to IDLE
    _nextRetryMs = 0;
    _state = State::Idle;
    _consecutiveErrors = 0;
    Serial.printf("[SHT31 0x%02X] retrying after sustained failure recovery window\n", _address);
  }

  switch (_state) {
    case State::Idle: {
      if (now - _lastReadMs < Battery::SHT31_INTERVAL_MS) return;
      // Issue single-shot high-repeatability measurement (no clock-stretch)
      if (!_sendCommand(0x2400)) {  // 0x2400 = no clock stretch, high repeatability
        _reading.status = Sht31Status::I2cError;
        if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          _nextRetryMs = now + RECOVERY_RETRY_MS;
          Serial.printf("[SHT31 0x%02X] %u consecutive errors — entering recovery cooldown (%lu s)\n",
                        _address, _consecutiveErrors, RECOVERY_RETRY_MS / 1000);
        }
        return;
      }
      _state = State::Converting;
      _stateEnteredMs = now;
      break;
    }
    case State::Converting: {
      if (now - _stateEnteredMs < SHT31_CONV_TIME_MS) return;  // not ready yet
      _state = State::Ready;
      // fall through to Ready state immediately to avoid an extra tick
      [[fallthrough]];
    }
    case State::Ready: {
      uint8_t buf[6];
      if (!_read6(buf)) {
        _reading.status = Sht31Status::I2cError;
        if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          _nextRetryMs = now + RECOVERY_RETRY_MS;
          Serial.printf("[SHT31 0x%02X] %u consecutive errors — entering recovery cooldown (%lu s)\n",
                        _address, _consecutiveErrors, RECOVERY_RETRY_MS / 1000);
        }
        _state = State::Idle;
        _lastReadMs = now;
        return;
      }
      if (!_crc8(buf, 2, buf[2]) || !_crc8(buf + 3, 2, buf[5])) {
        _reading.status = Sht31Status::CrcError;
        if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          _nextRetryMs = now + RECOVERY_RETRY_MS;
        }
        _state = State::Idle;
        _lastReadMs = now;
        return;
      }

      uint16_t rawT = ((uint16_t)buf[0] << 8) | buf[1];
      uint16_t rawRH = ((uint16_t)buf[3] << 8) | buf[4];
      float t = -45.0f + 175.0f * (rawT / 65535.0f);
      float rh = 100.0f * (rawRH / 65535.0f);
      if (rh < 0.0f) rh = 0.0f;
      if (rh > 100.0f) rh = 100.0f;

      if (t < -40.0f || t > 125.0f) {
        _reading.status = Sht31Status::OutOfRange;
        _state = State::Idle;
        _lastReadMs = now;
        return;
      }

      _reading.temperatureC = t;
      _reading.humidityRH = rh;
      _reading.timestamp = now;
      _reading.status = Sht31Status::Ok;
      _consecutiveErrors = 0;  // reset on success
      _state = State::Idle;
      _lastReadMs = now;
      break;
    }
  }
}

} // namespace Drivers

#endif // BATTERY_ENABLED
