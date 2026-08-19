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

// v4.3.1 audit D-007: Explicit safety lockout state machine per ChatGPT audit:
//   NORMAL → TRIPPED → ACKNOWLEDGED → CLEARED → ARMED → NORMAL
// Per audit: "Acknowledgement = operator mengetahui alarm. Bukan:
//   acknowledgement = sistem otomatis boleh menghidupkan relay."
enum class SafetyLockoutState : uint8_t {
  Normal       = 0,  // no safety trip, relay controllable
  Tripped       = 1,  // maxOnTime triggered, relay FORCED OFF, no re-enable
  Acknowledged = 2,  // operator saw alarm, lockout STILL ACTIVE
  Cleared      = 3,  // operator cleared lockout, relay still OFF, can re-enable
  Armed         = 4,  // system armed, normal operation resumes
};
inline const char* safetyLockoutStateStr(SafetyLockoutState s) {
  switch (s) {
    case SafetyLockoutState::Normal:       return "NORMAL";
    case SafetyLockoutState::Tripped:      return "TRIPPED";
    case SafetyLockoutState::Acknowledged: return "ACKNOWLEDGED";
    case SafetyLockoutState::Cleared:      return "CLEARED";
    case SafetyLockoutState::Armed:         return "ARMED";
  }
  return "NORMAL";
}

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

  // v4.3.1 D-007: Explicit safety state machine — ACK != CLEAR per ChatGPT audit.
  //   NORMAL → TRIPPED → ACKNOWLEDGED → CLEARED → ARMED → NORMAL
  // acknowledgeSafetyAlarm(): operator SEES the alarm (TRIPPED → ACKNOWLEDGED).
  //   Lockout STILL ACTIVE. Relay still forced OFF. No re-enable possible.
  // clearSafetyLockout(): operator explicitly clears (ACKNOWLEDGED → CLEARED).
  //   Relay still OFF but next manual command CAN re-enable.
  // armForNormalOperation(): CLEARED → ARMED → NORMAL (auto in tick).
  bool acknowledgeSafetyAlarm(uint8_t idx);   // TRIPPED → ACKNOWLEDGED (no clear!)
  bool clearSafetyLockout(uint8_t idx);        // ACKNOWLEDGED → CLEARED
  void armForNormalOperation(uint8_t idx);    // CLEARED → ARMED (auto in tick)

  // Query the current safety lockout state
  SafetyLockoutState getLockoutState(uint8_t idx) const;
  bool isSafetyLockoutActive(uint8_t idx) const;  // true if state != NORMAL && != ARMED

  // Reset all runtime tracking (e.g., on factory reset)
  void reset();

private:
  bool _bootStatesApplied = false;
  // v4.3.1 D-007: per-channel safety lockout state
  SafetyLockoutState _lockoutState[Core::NUM_CHANNELS] = {};
};

extern SafetySupervisor safety;

} // namespace Services

#endif // TIMER12_SAFETY_SUPERVISOR_H
