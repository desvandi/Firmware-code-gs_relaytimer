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
#include "LogService.h"
#include <cstdio>

namespace Services {

SafetySupervisor safety;

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
    if (ch.maxOnTimeSec == 0) continue;        // no limit configured
    if (!Core::relayState[i]) continue;        // not currently ON
    if (ch.maxOnTimeForced) continue;          // already forced OFF
    if (ch.onSinceMs == 0) continue;           // never turned ON (shouldn't happen)
    unsigned long elapsedSec = (now - ch.onSinceMs) / 1000UL;
    if (elapsedSec >= ch.maxOnTimeSec) {
      // v4.3.1 D-007: enter TRIPPED state (not just set flag)
      ch.maxOnTimeForced = true;
      _lockoutState[i] = SafetyLockoutState::Tripped;
      alarms.raise(Err::RELAY_MAX_ON_TIME, AlarmSeverity::Critical,
                   "maxOnTime exceeded — channel FORCE OFF (TRIPPED)");
      return i;
    }
  }
  // v4.3.1 D-007: advance CLEARED → ARMED → NORMAL for channels that have
  // been cleared by operator
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    armForNormalOperation(i);
  }
  return 0xFF;
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
  return true;
}

bool SafetySupervisor::clearSafetyLockout(uint8_t idx) {
  if (idx >= Core::NUM_CHANNELS) return false;
  // Can only CLEAR from ACKNOWLEDGED state
  if (_lockoutState[idx] != SafetyLockoutState::Acknowledged) {
    return false;  // must acknowledge first
  }
  // v4.3.2 BLOCKER-02: CLEAR requires fault condition to be resolved.
  // Per ChatGPT audit: "clearSafetyAlarm() harus menolak apabila:
  //   faultStillActive == true"
  // The "fault" here is: was the relay ON for too long (maxOnTime exceeded)?
  // If the relay is STILL ON (shouldn't be — it was forced OFF), or if
  // onSinceMs indicates the relay was ON recently and hasn't been OFF
  // long enough, we reject the CLEAR.
  //
  // maxOnTimeForced=true means the fault is still active.
  // We clear maxOnTimeForced HERE (transitioning to CLEARED), but only
  // if the relay is actually OFF (forced OFF already happened).
  if (Core::relayState[idx]) {
    // Relay still ON — fault not resolved. Reject CLEAR.
    char msg[80];
    snprintf(msg, sizeof(msg), "CH%u CLEAR rejected — relay still ON (fault active)", idx + 1);
    return false;
  }
  // Relay is OFF — fault condition resolved. Clear lockout.
  _lockoutState[idx] = SafetyLockoutState::Cleared;
  Core::channels[idx].maxOnTimeForced = false;
  alarms.clear(Err::RELAY_MAX_ON_TIME);
  return true;
}

void SafetySupervisor::armForNormalOperation(uint8_t idx) {
  if (idx >= Core::NUM_CHANNELS) return;
  if (_lockoutState[idx] == SafetyLockoutState::Cleared) {
    _lockoutState[idx] = SafetyLockoutState::Armed;
  } else if (_lockoutState[idx] == SafetyLockoutState::Armed) {
    _lockoutState[idx] = SafetyLockoutState::Normal;
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
