// =============================================================================
// Sht31Driver.cpp — SHT31 ambient T/RH (raw I²C, no library dep)
// =============================================================================
// SHT31 single-shot clock-stretching command 0x2C06 (high repeatability, clock
// stretch enabled) → wait 15 ms → read 6 bytes:
//   [T_msb, T_lsb, T_crc, RH_msb, RH_lsb, RH_crc]
//
// Conversion (Sensirion datasheet §4):
//   T_C = -45 + 175 * (rawT / 65535)
//   RH  = 100 * (rawRH / 65535)   (clamp 0..100)
//
// CRC-8: poly=0x31, init=0xFF
// =============================================================================
#include "Sht31Driver.h"
#include "Config.h"
#include <Wire.h>
#include <cmath>

namespace Drivers {

Sht31Driver sht31(Battery::SHT31_ADDR);

static constexpr uint32_t SHT31_STALE_MS = 5000;  // 5 s without read = stale

Sht31Driver::Sht31Driver(uint8_t address) : _address(address & 0x7F) {}

bool Sht31Driver::_sendCommand(uint16_t cmd) {
  Wire.beginTransmission(_address);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Sht31Driver::_read6(uint8_t out[6]) {
  if (Wire.requestFrom((int)_address, 6) != 6) return false;
  uint32_t start = millis();
  while (Wire.available() < 6) {
    if (millis() - start > 100) return false;
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

  // Soft-reset (command 0x30A2) then wait 1 ms
  if (!_sendCommand(0x30A2)) {
    Serial.printf("[SHT31 0x%02X] not present (soft-reset failed)\n", _address);
    return false;
  }
  delay(2);

  // Try one conversion to verify
  if (!_sendCommand(0x2C06)) {  // single-shot, high repeatability, clock-stretch
    _reading.status = Sht31Status::I2cError;
    Serial.printf("[SHT31 0x%02X] measure command failed\n", _address);
    return false;
  }
  delay(20);
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
  Serial.printf("[SHT31 0x%02X] initialized\n", _address);
  return true;
}

void Sht31Driver::tick() {
  if (!_available) return;
  unsigned long now = millis();
  if (now - _lastReadMs < Battery::SHT31_INTERVAL_MS) return;
  _lastReadMs = now;

  // Issue measurement
  if (!_sendCommand(0x2C06)) {
    _reading.status = Sht31Status::I2cError;
    return;
  }
  // 15 ms conversion time (high repeatability) — delay small but bounded.
  // The brief §44 forbids long blocking. 15 ms is acceptable (existing
  // PZEM driver uses 1 s read interval with similar Modbus delays).
  delay(15);

  uint8_t buf[6];
  if (!_read6(buf)) {
    _reading.status = Sht31Status::I2cError;
    return;
  }
  if (!_crc8(buf, 2, buf[2]) || !_crc8(buf + 3, 2, buf[5])) {
    _reading.status = Sht31Status::CrcError;
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
    return;
  }

  _reading.temperatureC = t;
  _reading.humidityRH = rh;
  _reading.timestamp = now;
  _reading.status = Sht31Status::Ok;
}

} // namespace Drivers
