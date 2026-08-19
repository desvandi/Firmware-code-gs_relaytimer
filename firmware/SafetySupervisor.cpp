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
      // Force OFF this channel
      ch.maxOnTimeForced = true;
      alarms.raise(Err::RELAY_MAX_ON_TIME, AlarmSeverity::Critical,
                   "maxOnTime exceeded — channel FORCE OFF");
      // Clear the force flag will require operator acknowledgement (future)
      return i;
    }
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

} // namespace Services
