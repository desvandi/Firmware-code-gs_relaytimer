// =============================================================================
// PzemDriver.h — PZEM-004T v3.0 AC power meter (self-contained Modbus-RTU)
// =============================================================================
// Measures: voltage, current, power, energy (kWh), frequency, power factor
// Plus: alarm checking, daily energy tracking, min/max stats, derived calcs
// Communication: UART (Modbus-RTU, 9600 baud)
// No external library needed — implements Modbus-RTU protocol directly.
// =============================================================================
#pragma once
#ifndef TIMER12_PZEM_DRIVER_H
#define TIMER12_PZEM_DRIVER_H

#include <Arduino.h>

namespace Drivers {

struct PzemData {
  float voltage;       // Volts AC
  float current;       // Amperes
  float power;         // Watts (active)
  float energy;        // kWh (accumulated, total since PZEM manufacture)
  float frequency;     // Hz
  float powerFactor;   // 0.0 - 1.0
  bool alarm;          // Over-current alarm (from PZEM hardware)
};

// Derived calculations
struct PzemDerived {
  float apparentPower;   // VA = V × A
  float reactivePower;   // VAR = √(VA² - W²)
};

// Daily statistics (reset at midnight)
struct PzemDailyStats {
  float energyStartKwh;  // Energy reading at start of day
  float energyTodayKwh;  // Energy consumed today
  float voltageMin;       // Minimum voltage today
  float voltageMax;       // Maximum voltage today
  float currentMax;       // Maximum current today
  float powerMax;         // Maximum power today
  float powerAvg;          // Average power today (running)
  uint32_t sampleCount;   // Number of samples today
  float powerSum;          // Sum of power for averaging
  uint8_t lastResetDay;   // Day of week when last reset (0=Mon)
};

// Alarm state
struct PzemAlarms {
  bool undervoltage;      // V < ALARM_VOLTAGE_MIN
  bool overvoltage;       // V > ALARM_VOLTAGE_MAX
  bool overcurrent;       // I > ALARM_CURRENT_MAX
  bool overpower;         // P > ALARM_POWER_MAX
  bool lowPowerFactor;    // PF < ALARM_PF_MIN
  unsigned long lastUnderVAlarmMs;
  unsigned long lastOverVAlarmMs;
  unsigned long lastOverIAlarmMs;
  unsigned long lastOverPAlarmMs;
  unsigned long lastLowPfAlarmMs;
};

class PzemDriver {
public:
  bool begin();
  void tick();
  bool isAvailable() const { return _available; }
  PzemData getData() const { return _data; }
  PzemDerived getDerived() const { return _derived; }
  PzemDailyStats getDailyStats() const { return _daily; }
  PzemAlarms getAlarms() const { return _alarms; }

  // Individual getters
  float getVoltage() const { return _data.voltage; }
  float getCurrent() const { return _data.current; }
  float getPower() const { return _data.power; }
  float getEnergy() const { return _data.energy; }
  float getFrequency() const { return _data.frequency; }
  float getPowerFactor() const { return _data.powerFactor; }
  bool hasAlarm() const { return _data.alarm; }

  // Derived getters
  float getApparentPower() const { return _derived.apparentPower; }
  float getReactivePower() const { return _derived.reactivePower; }

  // Daily stats getters
  float getEnergyToday() const { return _daily.energyTodayKwh; }
  float getVoltageMin() const { return _daily.voltageMin; }
  float getVoltageMax() const { return _daily.voltageMax; }
  float getCurrentMax() const { return _daily.currentMax; }
  float getPowerMax() const { return _daily.powerMax; }
  float getPowerAvg() const { return _daily.sampleCount > 0 ? _daily.powerSum / _daily.sampleCount : 0; }

  // Reset energy counter on PZEM hardware
  bool resetEnergy();
  // Reset daily stats (called at midnight)
  void resetDailyStats();

private:
  bool _available = false;
  PzemData _data = {0, 0, 0, 0, 0, 0, false};
  PzemDerived _derived = {0, 0};
  PzemDailyStats _daily = {0, 0, 999, 0, 0, 0, 0, 0, 0, 255};
  PzemAlarms _alarms = {false, false, false, false, false, 0, 0, 0, 0, 0};
  unsigned long _lastReadMs = 0;
  uint8_t _rxBuffer[25];
  uint8_t _rxIndex = 0;

  bool _readAll();
  bool _sendRequest(const uint8_t* data, uint8_t len);
  uint16_t _calculateCRC(const uint8_t* data, uint8_t len);
  bool _parseResponse();
  void _updateDerived();
  void _updateDailyStats();
  void _checkAlarms();
  void _triggerAlarm(const char* alarmType, const String& message, unsigned long& lastTriggerMs);
};

extern PzemDriver pzem;

} // namespace Drivers

#endif
