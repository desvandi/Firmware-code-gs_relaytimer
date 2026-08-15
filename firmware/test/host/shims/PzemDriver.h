// =============================================================================
// Stub: PzemDriver.h (overrides firmware/PzemDriver.h via -I shims)
// =============================================================================
// MqttClient.cpp uses Drivers::pzem.isAvailable() (in publishStatus) and
// .resetEnergy() / .resetDailyStats() (in system reset commands). The full
// firmware/PzemDriver.h declares the class with most methods inline, but
// .resetEnergy() / .resetDailyStats() are in PzemDriver.cpp (which uses
// UART — unavailable on host). We provide a minimal stub.
//
// Default behavior: isAvailable() returns false → publishStatus skips the
// PZEM data block entirely (matches "PZEM not connected" production state).
// =============================================================================
#pragma once
#ifndef HOST_SHIM_PZEM_DRIVER_H
#define HOST_SHIM_PZEM_DRIVER_H

#include <Arduino.h>

namespace Drivers {

struct PzemData {
  float voltage;
  float current;
  float power;
  float energy;
  float frequency;
  float powerFactor;
  bool alarm;
};

struct PzemDerived {
  float apparentPower;
  float reactivePower;
};

struct PzemDailyStats {
  float energyStartKwh;
  float energyTodayKwh;
  float voltageMin;
  float voltageMax;
  float currentMax;
  float powerMax;
  float powerAvg;
  uint32_t sampleCount;
  float powerSum;
  uint8_t lastResetDay;
};

struct PzemAlarms {
  bool undervoltage;
  bool overvoltage;
  bool overcurrent;
  bool overpower;
  bool lowPowerFactor;
  unsigned long lastUnderVAlarmMs;
  unsigned long lastOverVAlarmMs;
  unsigned long lastOverIAlarmMs;
  unsigned long lastOverPAlarmMs;
  unsigned long lastLowPfAlarmMs;
};

class PzemDriver {
public:
  bool begin() { return false; }
  void tick() {}
  bool isAvailable() const { return _available; }

  PzemData getData() const { return _data; }
  PzemDerived getDerived() const { return _derived; }
  PzemDailyStats getDailyStats() const { return _daily; }
  PzemAlarms getAlarms() const { return _alarms; }

  float getVoltage() const { return _data.voltage; }
  float getCurrent() const { return _data.current; }
  float getPower() const { return _data.power; }
  float getEnergy() const { return _data.energy; }
  float getFrequency() const { return _data.frequency; }
  float getPowerFactor() const { return _data.powerFactor; }
  bool hasAlarm() const { return _data.alarm; }

  float getApparentPower() const { return _derived.apparentPower; }
  float getReactivePower() const { return _derived.reactivePower; }

  float getEnergyToday() const { return _daily.energyTodayKwh; }
  float getVoltageMin() const { return _daily.voltageMin; }
  float getVoltageMax() const { return _daily.voltageMax; }
  float getCurrentMax() const { return _daily.currentMax; }
  float getPowerMax() const { return _daily.powerMax; }
  float getPowerAvg() const {
    return _daily.sampleCount > 0 ? _daily.powerSum / _daily.sampleCount : 0;
  }

  bool resetEnergy() { return true; }
  void resetDailyStats() {}

private:
  bool _available = false;
  PzemData _data = {0, 0, 0, 0, 0, 0, false};
  PzemDerived _derived = {0, 0};
  PzemDailyStats _daily = {0, 0, 999, 0, 0, 0, 0, 0, 0, 255};
  PzemAlarms _alarms = {false, false, false, false, false, 0, 0, 0, 0, 0};
};

extern PzemDriver pzem;

} // namespace Drivers

#endif // HOST_SHIM_PZEM_DRIVER_H
