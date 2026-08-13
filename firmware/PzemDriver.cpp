// =============================================================================
// PzemDriver.cpp — PZEM-004T v3.0 AC power meter (self-contained Modbus-RTU)
// =============================================================================
// Modbus-RTU protocol for PZEM-004T v3.0:
//   Read all registers:  [addr, 0x04, 0x00, 0x00, 0x00, 0x0A, CRC_L, CRC_H]
//   Response (25 bytes): [addr, 0x04, 0x14, data[20], CRC_L, CRC_H]
//
// Data layout (10 registers × 2 bytes = 20 bytes):
//   Reg 0 (2 bytes):   Voltage    (uint16, ×0.1 V)
//   Reg 1-2 (4 bytes): Current    (uint32, ×0.001 A)
//   Reg 3-4 (4 bytes): Power      (uint32, ×0.1 W)
//   Reg 5-6 (4 bytes): Energy     (uint32, ×0.001 kWh)
//   Reg 7 (2 bytes):   Frequency  (uint16, ×0.1 Hz)
//   Reg 8 (2 bytes):   Power Factor (uint16, ×0.01)
//   Reg 9 (2 bytes):   Alarm      (uint16, 0xFFFF = alarm)
// =============================================================================
#include "PzemDriver.h"
#include "Config.h"
#include "LogService.h"
#include "RtcDriver.h"
#include <HardwareSerial.h>

namespace Drivers {

PzemDriver pzem;

// HardwareSerial for UART1 (remapped to GPIO4/5)
// UART0 is used by USB Serial, UART2 default pins conflict with relays
HardwareSerial pzemSerial(1);

bool PzemDriver::begin() {
  // Initialize UART1 on custom pins: RX=GPIO5, TX=GPIO4
  pzemSerial.begin(Core::PZEM_BAUD_RATE, SERIAL_8N1,
                    Core::PZEM_RX_PIN, Core::PZEM_TX_PIN);
  delay(100);

  // Try initial read to detect if PZEM is connected
  Serial.printf("[PZEM] UART1 initialized: RX=GPIO%d, TX=GPIO%d, baud=%lu\n",
                Core::PZEM_RX_PIN, Core::PZEM_TX_PIN, Core::PZEM_BAUD_RATE);

  // Attempt first read
  bool ok = _readAll();
  if (ok) {
    Serial.println("[PZEM] PZEM-004T v3.0 detected!");
    Serial.printf("[PZEM] V=%.1fV, I=%.3fA, P=%.1fW, E=%.3fkWh, F=%.1fHz, PF=%.2f\n",
                  _data.voltage, _data.current, _data.power,
                  _data.energy, _data.frequency, _data.powerFactor);
    _available = true;
  } else {
    Serial.println("[PZEM] PZEM-004T v3.0 not found — power meter disabled");
    Serial.println("[PZEM] Check wiring: VCC→5V, GND→GND, TX→GPIO5, RX→GPIO4");
    _available = false;
  }
  return _available;
}

void PzemDriver::tick() {
  if (!_available) return;

  unsigned long now = millis();
  if (now - _lastReadMs < Core::PZEM_READ_INTERVAL_MS) return;
  _lastReadMs = now;

  if (_readAll()) {
    _updateDerived();
    _updateDailyStats();
    _checkAlarms();
  }
}

void PzemDriver::_updateDerived() {
  // Apparent Power: VA = V × A
  _derived.apparentPower = _data.voltage * _data.current;

  // Reactive Power: VAR = √(VA² - W²)
  float vaSq = _derived.apparentPower * _derived.apparentPower;
  float wSq = _data.power * _data.power;
  if (vaSq > wSq) {
    _derived.reactivePower = sqrt(vaSq - wSq);
  } else {
    _derived.reactivePower = 0;  // PF=1, no reactive load
  }
}

void PzemDriver::_updateDailyStats() {
  // Check if day changed (using RTC weekday)
  int y, m, d, h, mi, s, weekday;
  Drivers::rtc.getDateTime(y, m, d, h, mi, s, weekday);

  // Reset daily stats at midnight (or first read of new day)
  if (_daily.lastResetDay != (uint8_t)weekday) {
    if (_daily.lastResetDay != 255) {
      // Save yesterday's energy for reference
      Services::Log.append(Core::LogType::ConfigChange,
        "Daily reset: energy=" + String(_daily.energyTodayKwh, 3) + "kWh, " +
        "Vmin=" + String(_daily.voltageMin, 1) + "V, Vmax=" + String(_daily.voltageMax, 1) + "V, " +
        "Imax=" + String(_daily.currentMax, 3) + "A, Pmax=" + String(_daily.powerMax, 1) + "W", 0);
    }
    _daily.energyStartKwh = _data.energy;
    _daily.energyTodayKwh = 0;
    _daily.voltageMin = 999.0;
    _daily.voltageMax = 0;
    _daily.currentMax = 0;
    _daily.powerMax = 0;
    _daily.powerSum = 0;
    _daily.sampleCount = 0;
    _daily.lastResetDay = (uint8_t)weekday;
  }

  // Update energy today
  if (_daily.energyStartKwh > 0) {
    _daily.energyTodayKwh = _data.energy - _daily.energyStartKwh;
    if (_daily.energyTodayKwh < 0) _daily.energyTodayKwh = 0;  // PZEM was reset
  }

  // Update min/max
  if (_data.voltage > 0 && _data.voltage < _daily.voltageMin) _daily.voltageMin = _data.voltage;
  if (_data.voltage > _daily.voltageMax) _daily.voltageMax = _data.voltage;
  if (_data.current > _daily.currentMax) _daily.currentMax = _data.current;
  if (_data.power > _daily.powerMax) _daily.powerMax = _data.power;

  // Running average
  _daily.powerSum += _data.power;
  _daily.sampleCount++;
}

void PzemDriver::_checkAlarms() {
  unsigned long now = millis();

  // Undervoltage: V < 190V
  if (_data.voltage > 0 && _data.voltage < Core::ALARM_VOLTAGE_MIN) {
    if (!_alarms.undervoltage || (now - _alarms.lastUnderVAlarmMs > Core::ALARM_COOLDOWN_MS)) {
      _alarms.undervoltage = true;
      _triggerAlarm("undervoltage",
        "Undervoltage: " + String(_data.voltage, 1) + "V < " + String(Core::ALARM_VOLTAGE_MIN, 0) + "V",
        _alarms.lastUnderVAlarmMs);
    }
  } else {
    _alarms.undervoltage = false;
  }

  // Overvoltage: V > 250V
  if (_data.voltage > Core::ALARM_VOLTAGE_MAX) {
    if (!_alarms.overvoltage || (now - _alarms.lastOverVAlarmMs > Core::ALARM_COOLDOWN_MS)) {
      _alarms.overvoltage = true;
      _triggerAlarm("overvoltage",
        "Overvoltage: " + String(_data.voltage, 1) + "V > " + String(Core::ALARM_VOLTAGE_MAX, 0) + "V",
        _alarms.lastOverVAlarmMs);
    }
  } else {
    _alarms.overvoltage = false;
  }

  // Overcurrent: I > 8A
  if (_data.current > Core::ALARM_CURRENT_MAX) {
    if (!_alarms.overcurrent || (now - _alarms.lastOverIAlarmMs > Core::ALARM_COOLDOWN_MS)) {
      _alarms.overcurrent = true;
      _triggerAlarm("overcurrent",
        "Overcurrent: " + String(_data.current, 3) + "A > " + String(Core::ALARM_CURRENT_MAX, 1) + "A",
        _alarms.lastOverIAlarmMs);
    }
  } else {
    _alarms.overcurrent = false;
  }

  // Overpower: P > 1500W
  if (_data.power > Core::ALARM_POWER_MAX) {
    if (!_alarms.overpower || (now - _alarms.lastOverPAlarmMs > Core::ALARM_COOLDOWN_MS)) {
      _alarms.overpower = true;
      _triggerAlarm("overpower",
        "Overpower: " + String(_data.power, 1) + "W > " + String(Core::ALARM_POWER_MAX, 0) + "W",
        _alarms.lastOverPAlarmMs);
    }
  } else {
    _alarms.overpower = false;
  }

  // Low power factor: PF < 0.70
  if (_data.powerFactor > 0 && _data.powerFactor < Core::ALARM_PF_MIN) {
    if (!_alarms.lowPowerFactor || (now - _alarms.lastLowPfAlarmMs > Core::ALARM_COOLDOWN_MS)) {
      _alarms.lowPowerFactor = true;
      _triggerAlarm("low_pf",
        "Low Power Factor: " + String(_data.powerFactor, 2) + " < " + String(Core::ALARM_PF_MIN, 2) +
        " — inductive load detected",
        _alarms.lastLowPfAlarmMs);
    }
  } else {
    _alarms.lowPowerFactor = false;
  }
}

void PzemDriver::_triggerAlarm(const char* alarmType, const String& message, unsigned long& lastTriggerMs) {
  lastTriggerMs = millis();
  Services::Log.append(Core::LogType::Error, message, 0);
  Serial.printf("[PZEM ALARM] %s: %s\n", alarmType, message.c_str());
}

void PzemDriver::resetDailyStats() {
  _daily.energyStartKwh = _data.energy;
  _daily.energyTodayKwh = 0;
  _daily.voltageMin = 999.0;
  _daily.voltageMax = 0;
  _daily.currentMax = 0;
  _daily.powerMax = 0;
  _daily.powerSum = 0;
  _daily.sampleCount = 0;
  Services::Log.append(Core::LogType::ConfigChange, "Daily stats manually reset", 0);
}

bool PzemDriver::_readAll() {
  // Build Modbus request: read 10 holding registers starting from 0x0000
  uint8_t request[] = {
    Core::PZEM_MODBUS_ADDR,  // Slave address
    0x04,                     // Function code: Read Input Registers
    0x00, 0x00,               // Start address: 0x0000
    0x00, 0x0A,               // Number of registers: 10
    0x00, 0x00                // CRC placeholder (filled below)
  };

  // Calculate CRC
  uint16_t crc = _calculateCRC(request, 6);
  request[6] = crc & 0xFF;        // CRC low byte
  request[7] = (crc >> 8) & 0xFF; // CRC high byte

  // Send request
  if (!_sendRequest(request, 8)) {
    return false;
  }

  // Read response (25 bytes expected: addr + func + byteCount + 20 data + 2 CRC)
  _rxIndex = 0;
  unsigned long startMs = millis();

  while (_rxIndex < 25 && (millis() - startMs) < Core::PZEM_TIMEOUT_MS) {
    if (pzemSerial.available()) {
      _rxBuffer[_rxIndex++] = pzemSerial.read();
    } else {
      delay(1);
    }
  }

  if (_rxIndex < 25) {
    // Incomplete response
    if (_available) {
      // Only log error if previously available (avoid spam during init)
      Serial.printf("[PZEM] Response timeout: got %d/25 bytes\n", _rxIndex);
    }
    return false;
  }

  return _parseResponse();
}

bool PzemDriver::_sendRequest(const uint8_t* data, uint8_t len) {
  // Clear RX buffer before sending
  while (pzemSerial.available()) pzemSerial.read();

  // Small delay to ensure PZEM is ready (Modbus requires 3.5 char time gap)
  delay(10);

  pzemSerial.write(data, len);
  pzemSerial.flush();

  // Wait for transmission to complete
  delay(10);

  return true;
}

uint16_t PzemDriver::_calculateCRC(const uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

bool PzemDriver::_parseResponse() {
  // Verify slave address
  if (_rxBuffer[0] != Core::PZEM_MODBUS_ADDR) {
    Serial.printf("[PZEM] Wrong slave address: 0x%02X\n", _rxBuffer[0]);
    return false;
  }

  // Verify function code
  if (_rxBuffer[1] != 0x04) {
    Serial.printf("[PZEM] Wrong function code: 0x%02X\n", _rxBuffer[1]);
    return false;
  }

  // Verify byte count
  if (_rxBuffer[2] != 0x14) {  // 20 bytes of data
    Serial.printf("[PZEM] Wrong byte count: %d\n", _rxBuffer[2]);
    return false;
  }

  // Verify CRC
  uint16_t calcCrc = _calculateCRC(_rxBuffer, 23);
  uint16_t respCrc = _rxBuffer[23] | (_rxBuffer[24] << 8);
  if (calcCrc != respCrc) {
    Serial.printf("[PZEM] CRC mismatch: calc=0x%04X, resp=0x%04X\n", calcCrc, respCrc);
    return false;
  }

  // Parse data (big-endian in Modbus)
  uint8_t* d = &_rxBuffer[3];  // Start of data bytes

  // Register 0: Voltage (uint16, ×0.1 V)
  uint16_t voltageRaw = (d[0] << 8) | d[1];
  _data.voltage = voltageRaw * 0.1;

  // Register 1-2: Current (uint32, ×0.001 A)
  uint32_t currentRaw = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) |
                        ((uint32_t)d[4] << 8) | d[5];
  _data.current = currentRaw * 0.001;

  // Register 3-4: Power (uint32, ×0.1 W)
  uint32_t powerRaw = ((uint32_t)d[6] << 24) | ((uint32_t)d[7] << 16) |
                      ((uint32_t)d[8] << 8) | d[9];
  _data.power = powerRaw * 0.1;

  // Register 5-6: Energy (uint32, ×0.001 kWh)
  uint32_t energyRaw = ((uint32_t)d[10] << 24) | ((uint32_t)d[11] << 16) |
                       ((uint32_t)d[12] << 8) | d[13];
  _data.energy = energyRaw * 0.001;

  // Register 7: Frequency (uint16, ×0.1 Hz)
  uint16_t freqRaw = (d[14] << 8) | d[15];
  _data.frequency = freqRaw * 0.1;

  // Register 8: Power Factor (uint16, ×0.01)
  uint16_t pfRaw = (d[16] << 8) | d[17];
  _data.powerFactor = pfRaw * 0.01;

  // Register 9: Alarm (uint16, 0xFFFF = alarm triggered)
  uint16_t alarmRaw = (d[18] << 8) | d[19];
  _data.alarm = (alarmRaw == 0xFFFF);

  return true;
}

bool PzemDriver::resetEnergy() {
  // Custom command to reset energy counter (not standard Modbus)
  uint8_t request[] = {
    Core::PZEM_MODBUS_ADDR,
    0x42,   // Custom function code for energy reset
    0x00, 0x00  // CRC placeholder
  };

  uint16_t crc = _calculateCRC(request, 2);
  request[2] = crc & 0xFF;
  request[3] = (crc >> 8) & 0xFF;

  if (!_sendRequest(request, 4)) {
    return false;
  }

  // Wait for response
  _rxIndex = 0;
  unsigned long startMs = millis();
  while (_rxIndex < 4 && (millis() - startMs) < Core::PZEM_TIMEOUT_MS) {
    if (pzemSerial.available()) {
      _rxBuffer[_rxIndex++] = pzemSerial.read();
    } else {
      delay(1);
    }
  }

  if (_rxIndex >= 4) {
    Services::Log.append(Core::LogType::ConfigChange, "PZEM energy reset", 0);
    return true;
  }
  return false;
}

} // namespace Drivers
