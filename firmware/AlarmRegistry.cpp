// =============================================================================
// AlarmRegistry.cpp — Central alarm engine implementation (brief §60)
// =============================================================================
#include "AlarmRegistry.h"
#include <cstring>
#include <cstdio>

namespace Services {

AlarmRegistry alarms;

void AlarmRegistry::begin() {
  _count = 0;
  for (uint8_t i = 0; i < MAX_ALARMS; i++) {
    _alarms[i] = {};
    _alarms[i].code[0] = '\0';
  }
}

uint8_t AlarmRegistry::_findIdx(const char* code) const {
  if (!code) return 0xFF;
  for (uint8_t i = 0; i < _count; i++) {
    if (strncmp(_alarms[i].code, code, Alarm::CODE_LEN) == 0) return i;
  }
  return 0xFF;
}

void AlarmRegistry::raise(const char* code, AlarmSeverity sev, const char* message) {
  if (!code) return;
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) {
    // New alarm
    if (_count >= MAX_ALARMS) {
      // Registry full — drop oldest cleared alarm to make space
      uint8_t oldest = 0xFF;
      uint32_t oldestTime = 0xFFFFFFFF;
      for (uint8_t i = 0; i < _count; i++) {
        if (!_alarms[i].active && _alarms[i].clearedAt < oldestTime) {
          oldestTime = _alarms[i].clearedAt;
          oldest = i;
        }
      }
      if (oldest != 0xFF) {
        // Shift down
        for (uint8_t i = oldest; i < _count - 1; i++) {
          _alarms[i] = _alarms[i + 1];
        }
        _count--;
        idx = _count;
      } else {
        // No cleared alarms — drop the lowest-severity oldest
        idx = 0;
        for (uint8_t i = 1; i < _count; i++) {
          if ((uint8_t)_alarms[i].severity < (uint8_t)_alarms[idx].severity) idx = i;
        }
      }
    }
    Alarm& a = _alarms[idx];
    strncpy(a.code, code, Alarm::CODE_LEN - 1);
    a.code[Alarm::CODE_LEN - 1] = '\0';
    a.severity = sev;
    a.active = true;
    a.acknowledged = false;
    a.raisedAt = millis();
    a.clearedAt = 0;
    a.lastUpdatedAt = a.raisedAt;
    if (message) {
      strncpy(a.message, message, sizeof(a.message) - 1);
      a.message[sizeof(a.message) - 1] = '\0';
    } else {
      a.message[0] = '\0';
    }
    if (idx == _count) _count++;

    // Log to activity log (avoid spam — only on raise, not on refresh)
    char buf[96];
    snprintf(buf, sizeof(buf), "[ALARM:%s] %s — %s",
             alarmSeverityStr(sev), code, message ? message : "");
    // Note: caller may want to also write to Core::Log; we keep AlarmRegistry
    // decoupled from LogService to avoid circular include.
  } else {
    // Existing alarm — refresh
    Alarm& a = _alarms[idx];
    a.severity = sev;  // upgrade if needed (never downgrade)
    a.lastUpdatedAt = millis();
    if (message && message[0]) {
      strncpy(a.message, message, sizeof(a.message) - 1);
      a.message[sizeof(a.message) - 1] = '\0';
    }
  }
}

void AlarmRegistry::clear(const char* code) {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return;
  _alarms[idx].active = false;
  _alarms[idx].clearedAt = millis();
  _alarms[idx].lastUpdatedAt = _alarms[idx].clearedAt;
}

void AlarmRegistry::acknowledge(const char* code) {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return;
  _alarms[idx].acknowledged = true;
}

void AlarmRegistry::acknowledgeAll() {
  for (uint8_t i = 0; i < _count; i++) {
    _alarms[i].acknowledged = true;
  }
}

uint8_t AlarmRegistry::countActive() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].active) n++;
  }
  return n;
}

uint8_t AlarmRegistry::countAll() const {
  return _count;
}

const Alarm* AlarmRegistry::getAlarm(uint8_t idx) const {
  if (idx >= _count) return nullptr;
  return &_alarms[idx];
}

const Alarm* AlarmRegistry::find(const char* code) const {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return nullptr;
  return &_alarms[idx];
}

AlarmSeverity AlarmRegistry::highestActiveSeverity() const {
  AlarmSeverity highest = AlarmSeverity::Info;
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].active && (uint8_t)_alarms[i].severity > (uint8_t)highest) {
      highest = _alarms[i].severity;
    }
  }
  return highest;
}

} // namespace Services
