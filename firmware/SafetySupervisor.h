// =============================================================================
// SafetySupervisor.h — Per-channel relay safety enforcement (brief §13-16)
// Timer Digital Relay v4.2 — Industrial-Grade Hardening
// -----------------------------------------------------------------------------
// Enforces per-channel safety limits BEFORE any relay GPIO write:
//   • maxOnTime (§14) — force OFF after configured seconds
//   • minOnTime (§15) — inhibit OFF before configured seconds elapsed
//   • minOffTime (§15) — inhibit ON before configured seconds elapsed
//   • minSwitchInterval / anti-chatter (§16) — block rapid ON/OFF cycles
//   • boot policy (§13) — applies safe state at boot
//
// All enforcement is LOCAL-FIRST (brief §5, §78) — works without MQTT,
// Internet, PWA, or GAS. Safety is non-negotiable (brief §113).
//
// Returns a Decision enum that RelayEngine consults before applying state.
// =============================================================================
#pragma once
#ifndef TIMER12_SAFETY_SUPERVISOR_H
#define TIMER12_SAFETY_SUPERVISOR_H

#include <Arduino.h>
#include <cstdint>
#include "Types.h"
#include "AlarmRegistry.h"
#include "ErrorCodes.h"

namespace Services {

enum class SafetyDecision : uint8_t {
  Allow          = 0,  // Transition permitted
  InhibitMinOn   = 1,  // Block OFF — minOnTime not yet elapsed
  InhibitMinOff  = 2,  // Block ON — minOffTime not yet elapsed
  InhibitChatter = 3,  // Block transition — too rapid (anti-chatter)
  ForceOffMaxOn  = 4,  // Force OFF — maxOnTime exceeded
  RejectBoot     = 5,  // Boot policy rejects requested state
};

inline const char* safetyDecisionStr(SafetyDecision d) {
  switch (d) {
    case SafetyDecision::Allow:           return "ALLOW";
    case SafetyDecision::InhibitMinOn:   return "INHIBIT_MIN_ON";
    case SafetyDecision::InhibitMinOff:  return "INHIBIT_MIN_OFF";
    case SafetyDecision::InhibitChatter: return "INHIBIT_CHATTER";
    case SafetyDecision::ForceOffMaxOn:  return "FORCE_OFF_MAX_ON";
    case SafetyDecision::RejectBoot:     return "REJECT_BOOT";
  }
  return "UNKNOWN";
}

class SafetySupervisor {
public:
  // Called once at boot — applies boot policy per channel (brief §13).
  // Returns the desired boot state for each channel given its policy +
  // last-known state (for RESTORE_LAST).
  bool computeBootState(uint8_t idx, bool lastKnownState);

  // Called from RelayEngine::tick() before applying any transition.
  // `idx` is the channel index (0..NUM_CHANNELS-1).
  // `desired` is what CommandArbiter computed (manual/schedule/PIR result).
  // `current` is the current physical relay state.
  // Returns the safety decision — RelayEngine must respect it.
  SafetyDecision evaluateTransition(uint8_t idx, bool desired, bool current);

  // Called from RelayEngine::tick() AFTER a transition is applied, to
  // update internal tracking (lastTransitionMs, onSinceMs, etc.).
  void recordTransition(uint8_t idx, bool newState);

  // Called periodically from tick() to check maxOnTime on currently-ON
  // channels. Returns the channel index that should be force-OFF, or
  // 0xFF if none. Called in a loop until it returns 0xFF.
  uint8_t checkMaxOnTimeExceeded();

  // v4.3 audit P1-003: EXPLICIT safety alarm acknowledgement.
  // Manual relay commands (setManual) NO LONGER auto-clear maxOnTimeForced.
  // Operator must call this explicitly to clear the safety lockout.
  // Returns true if the alarm was cleared; false if no lockout was active.
  bool acknowledgeSafetyAlarm(uint8_t idx);

  // v4.3 audit P1-003: Check whether a channel is in safety lockout.
  bool isSafetyLockoutActive(uint8_t idx) const;

  // Reset all runtime tracking (e.g., on factory reset)
  void reset();

private:
  bool _bootStatesApplied = false;
};

extern SafetySupervisor safety;

} // namespace Services

#endif // TIMER12_SAFETY_SUPERVISOR_H
