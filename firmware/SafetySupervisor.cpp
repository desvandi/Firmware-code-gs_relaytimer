// =============================================================================
// SafetySupervisor.cpp — Per-channel relay safety enforcement (brief §13-16)
// =============================================================================
// Decision logic:
//   1. If current == desired → Allow (no transition)
//   2. If maxOnTime exceeded on a currently-ON channel → ForceOffMaxOn
//      (regardless of desired state — safety override)
//   3. If desired == ON && minOffTime not elapsed → InhibitMinOff
//   4. If desired == OFF && minOnTime not elapsed → InhibitMinOn
//   5. If minSwitchInterval not elapsed since last transition → InhibitChatter
//   6. Otherwise → Allow
//
// Alarms raised (brief §60):
//   RELAY_MAX_ON_TIME — when ForceOffMaxOn triggered
//   RELAY_MIN_ON_TIME — when InhibitMinOn blocks an OFF (informational)
//   RELAY_MIN_OFF_TIME — when InhibitMinOff blocks an ON
//   RELAY_ANTI_CHATTER — when InhibitChatter blocks any transition
// =============================================================================
#include "SafetySupervisor.h"
#include "Globals.h"  // v4.3.8 D-019 FIX: Core::channels[], Core::relayState[] etc. declared here
#include "LogService.h"
#include <cstdio>
#include <Preferences.h>  // AUD-FW-CFG-002 FIX: NVS persistence for lockout state

namespace Services {

SafetySupervisor safety;

// AUD-FW-CFG-002 FIX: NVS persistence for safety lockout state.
// Previously _lockoutState[] was RAM-only — a TRIPPED channel would appear
// as NORMAL after reboot, allowing operator to bypass ACK→CLEAR→ARM by
// power-cycling. Now persisted to NVS namespace "t12_safety".
static constexpr const char* SAFETY_NVS_NAMESPACE = "t12_safety";
static constexpr const char* SAFETY_NVS_KEY_VERSION = "ver";
static constexpr const char* SAFETY_NVS_KEY_LOCKOUT = "lockout";
static constexpr const char* SAFETY_NVS_KEY_FAULT = "fault";
static constexpr const char* SAFETY_NVS_KEY_REASON = "reason_";
static constexpr uint8_t SAFETY_SCHEMA_VERSION = 1;

void SafetySupervisor::begin() {
  Preferences prefs;
  if (!prefs.begin(SAFETY_NVS_NAMESPACE, true)) {
    Serial.println("[Safety] NVS open failed (read) — starting with default states");
    return;
  }

  uint8_t storedVer = prefs.getUChar(SAFETY_NVS_KEY_VERSION, 0);
  if (storedVer != SAFETY_SCHEMA_VERSION) {
    Serial.printf("[Safety] NVS schema mismatch (stored=%u, expected=%u) — starting fresh\n",
                  storedVer, SAFETY_SCHEMA_VERSION);
    prefs.end();
    // Clear + write version
    if (prefs.begin(SAFETY_NVS_NAMESPACE, false)) {
      prefs.putUChar(SAFETY_NVS_KEY_VERSION, SAFETY_SCHEMA_VERSION);
      prefs.end();
    }
    return;
  }

  // Load lockout states (12 bytes — one uint8_t per channel)
  size_t needed = prefs.getBytesLength(SAFETY_NVS_KEY_LOCKOUT);
  if (needed == Core::NUM_CHANNELS) {
    prefs.getBytes(SAFETY_NVS_KEY_LOCKOUT, _lockoutState, Core::NUM_CHANNELS);
  }

  // Load fault active flags (12 bytes)
  needed = prefs.getBytesLength(SAFETY_NVS_KEY_FAULT);
  if (needed == Core::NUM_CHANNELS) {
    prefs.getBytes(SAFETY_NVS_KEY_FAULT, _faultActive, Core::NUM_CHANNELS);
  }

  // Load fault reasons (12 × 32 bytes)
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", SAFETY_NVS_KEY_REASON, i);
    needed = prefs.getBytesLength(key);
    if (needed > 0 && needed <= 32) {
      prefs.getBytes(key, _faultReason[i], needed);
      _faultReason[i][31] = '\0';  // ensure null-terminated
    }
  }

  prefs.end();
  Serial.println("[Safety] Loaded lockout states from NVS");

  // Log any restored non-normal states
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    if (_lockoutState[i] != SafetyLockoutState::Normal) {
      Serial.printf("[Safety] CH%d restored to %s state (faultActive=%d, reason='%s')\n",
                    i + 1, safetyLockoutStateStr(_lockoutState[i]),
                    _faultActive[i] ? 1 : 0, _faultReason[i]);
    }
  }
}

void SafetySupervisor::_persistLockoutToNVS() {
  Preferences prefs;
  if (!prefs.begin(SAFETY_NVS_NAMESPACE, false)) {
    Serial.println("[Safety] NVS open failed (write) — lockout state NOT persisted");
    return;
  }

  prefs.putUChar(SAFETY_NVS_KEY_VERSION, SAFETY_SCHEMA_VERSION);
  prefs.putBytes(SAFETY_NVS_KEY_LOCKOUT, _lockoutState, Core::NUM_CHANNELS);
  prefs.putBytes(SAFETY_NVS_KEY_FAULT, _faultActive, Core::NUM_CHANNELS);
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", SAFETY_NVS_KEY_REASON, i);
    prefs.putBytes(key, _faultReason[i], 32);
  }

  prefs.end();
}

void SafetySupervisor::_loadLockoutFromNVS() {
  // Alias — actual loading is done in begin() which is the public entry point.
  // This private method exists for the header declaration; it calls begin()
  // which does the actual work. (Kept for API symmetry with other persistence
  // classes like TelemetrySpool.)
  begin();
}

bool SafetySupervisor::computeBootState(uint8_t idx, bool lastKnownState) {
  if (idx >= Core::NUM_CHANNELS) return false;
  uint8_t policy = Core::channels[idx].bootPolicy;
  switch (policy) {
    case (uint8_t)Core::BootPolicy::BootOff:
      return false;
    case (uint8_t)Core::BootPolicy::BootOn:
      return true;
    case (uint8_t)Core::BootPolicy::RestoreLast:
      return lastKnownState;
    case (uint8_t)Core::BootPolicy::SafeState:
    default:
      // Safe state defaults to OFF for hazardous loads
      return false;
  }
}

SafetyDecision SafetySupervisor::evaluateTransition(uint8_t idx, bool desired, bool current) {
  if (idx >= Core::NUM_CHANNELS) return SafetyDecision::Allow;
  if (desired == current) return SafetyDecision::Allow;

  unsigned long now = millis();
  Core::Channel& ch = Core::channels[idx];

  // §14: maxOnTime check — if currently ON and limit exceeded, force OFF
  // (this happens via checkMaxOnTimeExceeded() called from tick(), but we
  // also gate here to ensure no transition can re-enable a forced-OFF channel
  // until the operator clears the alarm)
  if (ch.maxOnTimeForced && desired) {
    alarms.raise(Err::RELAY_MAX_ON_TIME, AlarmSeverity::Warning,
                 "MaxOnTime force-OFF active — re-enable blocked until alarm cleared");
    return SafetyDecision::ForceOffMaxOn;
  }

  // §15: minOffTime — inhibit ON if not enough time since last OFF
  if (desired && ch.minOffTimeSec > 0 && ch.lastTransitionMs > 0) {
    unsigned long elapsedSec = (now - ch.lastTransitionMs) / 1000UL;
    if (elapsedSec < ch.minOffTimeSec) {
      // Don't spam alarm — raise once per inhibit window
      static unsigned long lastMinOffAlarm = 0;
      if (now - lastMinOffAlarm > 5000) {  // 5s throttle
        alarms.raise(Err::RELAY_MIN_OFF_TIME, AlarmSeverity::Info,
                     "ON inhibited — minOffTime not yet elapsed");
        lastMinOffAlarm = now;
      }
      return SafetyDecision::InhibitMinOff;
    }
  }

  // §15: minOnTime — inhibit OFF if not enough time since last ON
  if (!desired && ch.minOnTimeSec > 0 && ch.onSinceMs > 0) {
    unsigned long elapsedSec = (now - ch.onSinceMs) / 1000UL;
    if (elapsedSec < ch.minOnTimeSec) {
      static unsigned long lastMinOnAlarm = 0;
      if (now - lastMinOnAlarm > 5000) {
        alarms.raise(Err::RELAY_MIN_ON_TIME, AlarmSeverity::Info,
                     "OFF inhibited — minOnTime not yet elapsed");
        lastMinOnAlarm = now;
      }
      return SafetyDecision::InhibitMinOn;
    }
  }

  // §16: anti-chatter — block rapid transitions
  if (ch.minSwitchIntervalSec > 0 && ch.lastTransitionMs > 0) {
    unsigned long elapsedSec = (now - ch.lastTransitionMs) / 1000UL;
    if (elapsedSec < ch.minSwitchIntervalSec) {
      static unsigned long lastChatterAlarm = 0;
      if (now - lastChatterAlarm > 5000) {
        alarms.raise(Err::RELAY_ANTI_CHATTER, AlarmSeverity::Info,
                     "Transition blocked — anti-chatter interval not elapsed");
        lastChatterAlarm = now;
      }
      return SafetyDecision::InhibitChatter;
    }
  }

  return SafetyDecision::Allow;
}

void SafetySupervisor::recordTransition(uint8_t idx, bool newState) {
  if (idx >= Core::NUM_CHANNELS) return;
  Core::Channel& ch = Core::channels[idx];
  unsigned long now = millis();
  ch.lastTransitionMs = now;
  if (newState) {
    ch.onSinceMs = now;
  } else {
    ch.onSinceMs = 0;
  }
}

uint8_t SafetySupervisor::checkMaxOnTimeExceeded() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    Core::Channel& ch = Core::channels[i];
    if (ch.maxOnTimeSec == 0) continue;
    if (!Core::relayState[i]) continue;
    if (ch.maxOnTimeForced) continue;
    if (ch.onSinceMs == 0) continue;
    unsigned long elapsedSec = (now - ch.onSinceMs) / 1000UL;
    if (elapsedSec >= ch.maxOnTimeSec) {
      ch.maxOnTimeForced = true;
      _lockoutState[i] = SafetyLockoutState::Tripped;
      // v4.3.2 BLOCKER-02: set EXPLICIT fault tracking
      _faultActive[i] = true;
      strncpy(_faultReason[i], "maxOnTime exceeded", sizeof(_faultReason[i]) - 1);
      _faultReason[i][sizeof(_faultReason[i]) - 1] = '\0';
      alarms.raise(Err::RELAY_MAX_ON_TIME, AlarmSeverity::Critical,
                   "maxOnTime exceeded — channel FORCE OFF (TRIPPED)");
      _persistLockoutToNVS();  // AUD-FW-CFG-002 FIX: persist TRIPPED state
      return i;
    }
  }
  // v4.3.2 BLOCKER-02: check if fault condition has resolved for channels
  // in TRIPPED or ACKNOWLEDGED state. For maxOnTime: fault resolves when
  // relay goes OFF (no longer timing).
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    if (_faultActive[i] && _isFaultConditionResolved(i)) {
      _faultActive[i] = false;  // fault condition resolved
      // Do NOT auto-clear lockout — operator must still CLEAR explicitly.
      // This just means the precondition for CLEAR is now met.
    }
  }
  // Advance CLEARED → ARMED → NORMAL
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    armForNormalOperation(i);
  }
  return 0xFF;
}

bool SafetySupervisor::_isFaultConditionResolved(uint8_t idx) const {
  if (idx >= Core::NUM_CHANNELS) return false;
  if (!_faultActive[idx]) return true;  // no fault → trivially resolved
  // For maxOnTime fault: resolved when relay is OFF
  // (the timer can only accumulate while relay is ON)
  if (strncmp(_faultReason[idx], "maxOnTime", 9) == 0) {
    return !Core::relayState[idx];  // fault resolved when relay OFF
  }
  // Unknown fault type — conservatively require manual resolution
  return false;
}

void SafetySupervisor::reset() {
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    Core::channels[i].lastTransitionMs = 0;
    Core::channels[i].onSinceMs = 0;
    Core::channels[i].maxOnTimeForced = false;
  }
  _bootStatesApplied = false;
}

// v4.3.1 D-007: Explicit safety state machine per ChatGPT audit.
//   NORMAL → TRIPPED → ACKNOWLEDGED → CLEARED → ARMED → NORMAL
//
// Per audit: "Acknowledgement = operator mengetahui alarm. Bukan:
//   acknowledgement = sistem otomatis boleh menghidupkan relay."
//
// State transition rules:
//   NORMAL → TRIPPED: automatic when checkMaxOnTimeExceeded() detects limit
//   TRIPPED → ACKNOWLEDGED: only via acknowledgeSafetyAlarm() call
//   ACKNOWLEDGED → CLEARED: only via clearSafetyLockout() call
//   CLEARED → ARMED: automatic on next tick() (after clearing maxOnTimeForced)
//   ARMED → NORMAL: automatic on next tick() (no new trip)
//
// maxOnTimeForced flag is set when entering TRIPPED, cleared when entering
// CLEARED. Between TRIPPED and CLEARED, all ON commands are blocked.
bool SafetySupervisor::acknowledgeSafetyAlarm(uint8_t idx) {
  if (idx >= Core::NUM_CHANNELS) return false;
  // Can only ACK from TRIPPED state
  if (_lockoutState[idx] != SafetyLockoutState::Tripped) {
    return false;  // not in tripped state, nothing to acknowledge
  }
  _lockoutState[idx] = SafetyLockoutState::Acknowledged;
  alarms.acknowledge(Err::RELAY_MAX_ON_TIME);
  // maxOnTimeForced STAYS TRUE — relay still forced OFF
  // BLOCKER-02: ACK ≠ permission. Fault condition (maxOnTimeForced) still active.
  // CLEAR requires explicit clearSafetyLockout() call AFTER fault is resolved.
  char msg[80];
  snprintf(msg, sizeof(msg), "CH%u safety alarm acknowledged (still locked)", idx + 1);
  _persistLockoutToNVS();  // AUD-FW-CFG-002 FIX: persist ACKNOWLEDGED state
  return true;
}

bool SafetySupervisor::clearSafetyLockout(uint8_t idx) {
  if (idx >= Core::NUM_CHANNELS) return false;
  if (_lockoutState[idx] != SafetyLockoutState::Acknowledged) {
    return false;  // must acknowledge first
  }
  // v4.3.2 BLOCKER-02: CLEAR requires EXPLICIT fault condition to be resolved.
  if (_faultActive[idx] && !_isFaultConditionResolved(idx)) {
    return false;  // fault condition still active — cannot CLEAR
  }
  _lockoutState[idx] = SafetyLockoutState::Cleared;
  _faultActive[idx] = false;
  Core::channels[idx].maxOnTimeForced = false;
  alarms.clear(Err::RELAY_MAX_ON_TIME);
  _persistLockoutToNVS();  // AUD-FW-CFG-002 FIX: persist CLEARED state
  return true;
}

void SafetySupervisor::armForNormalOperation(uint8_t idx) {
  if (idx >= Core::NUM_CHANNELS) return;
  if (_lockoutState[idx] == SafetyLockoutState::Cleared) {
    _lockoutState[idx] = SafetyLockoutState::Armed;
    _persistLockoutToNVS();  // AUD-FW-CFG-002 FIX: persist ARMED state
  } else if (_lockoutState[idx] == SafetyLockoutState::Armed) {
    _lockoutState[idx] = SafetyLockoutState::Normal;
    _persistLockoutToNVS();  // AUD-FW-CFG-002 FIX: persist NORMAL state (lockout cleared)
  }
}

SafetyLockoutState SafetySupervisor::getLockoutState(uint8_t idx) const {
  if (idx >= Core::NUM_CHANNELS) return SafetyLockoutState::Normal;
  return _lockoutState[idx];
}

bool SafetySupervisor::isSafetyLockoutActive(uint8_t idx) const {
  if (idx >= Core::NUM_CHANNELS) return false;
  SafetyLockoutState s = _lockoutState[idx];
  return s != SafetyLockoutState::Normal && s != SafetyLockoutState::Armed;
}

} // namespace Services
