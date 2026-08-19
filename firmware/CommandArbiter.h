// =============================================================================
// CommandArbiter.h — Formal Command Arbitration Engine (audit P1-001)
// Timer Digital Relay v4.3 — Industrial-Grade Hardening Round 2
// -----------------------------------------------------------------------------
// Per ChatGPT audit (targeted remediation):
//   "Buat CommandArbiter yang menghasilkan:
//      ArbitrationResult {
//        targetState, source, priority, reason, requestId
//      }
//    Kemudian: Command → Arbiter → SafetySupervisor → RelayEngine → GPIO"
//
// This module replaces the hardcoded precedence logic in RelayEngine::_computeChannel
// with a formal arbitration engine. Sources are explicit + priority is numeric.
// =============================================================================
#pragma once
#ifndef TIMER12_COMMAND_ARBITER_H
#define TIMER12_COMMAND_ARBITER_H

#include <Arduino.h>
#include <cstdint>
#include "Types.h"
#include "Config.h"

namespace Services {

// ---------- COMMAND SOURCES (audit brief §8) ----------
enum class CommandSource : uint8_t {
  Safety        = 0,  // 1000 — maxOnTime FORCE OFF, interlock, fault
  Emergency     = 1,  //  900 — interlock emergency
  ManualAuth    = 2,  //  800 — operator manual command (authorized)
  Maintenance   = 3,  //  700 — maintenance mode override
  RemoteAuto    = 4,  //  600 — remote automation (PWA/GAS rule)
  Schedule      = 5,  //  500 — RTC-based schedule
  Pir           = 6,  //  400 — PIR motion override
  Default       = 7,  //  100 — default OFF
};

inline uint16_t sourcePriority(CommandSource s) {
  switch (s) {
    case CommandSource::Safety:      return 1000;
    case CommandSource::Emergency:   return 900;
    case CommandSource::ManualAuth:  return 800;
    case CommandSource::Maintenance: return 700;
    case CommandSource::RemoteAuto:  return 600;
    case CommandSource::Schedule:    return 500;
    case CommandSource::Pir:         return 400;
    case CommandSource::Default:     return 100;
  }
  return 0;
}

inline const char* sourceStr(CommandSource s) {
  switch (s) {
    case CommandSource::Safety:      return "safety";
    case CommandSource::Emergency:    return "emergency";
    case CommandSource::ManualAuth:   return "manual";
    case CommandSource::Maintenance:  return "maintenance";
    case CommandSource::RemoteAuto:   return "remote_auto";
    case CommandSource::Schedule:     return "schedule";
    case CommandSource::Pir:          return "pir";
    case CommandSource::Default:      return "off";
  }
  return "off";
}

// ---------- COMMAND SEMANTICS (audit P1-007) ----------
// Per ChatGPT audit: "logical idempotency ≠ physical side-effect idempotency"
// Firmware must distinguish SET_STATE (idempotent — re-executing is safe)
// from NON_IDEMPOTENT_ACTION (PULSE / TOGGLE / START_MOTOR /
// TRIGGER_CONTACTOR / RESET — re-executing is NOT safe).
enum class CommandSemantics : uint8_t {
  IdempotentState       = 0,  // SET_STATE: ON, OFF, SET_MODE — replay-safe
  NonIdempotentAction   = 1,  // PULSE / TOGGLE / etc. — NOT replay-safe
};

inline const char* semanticsStr(CommandSemantics s) {
  switch (s) {
    case CommandSemantics::IdempotentState:      return "IDEMPOTENT_STATE";
    case CommandSemantics::NonIdempotentAction:  return "NON_IDEMPOTENT_ACTION";
  }
  return "IDEMPOTENT_STATE";
}

// ---------- ARBITRATION RESULT ----------
struct ArbitrationResult {
  bool            targetState;     // desired ON/OFF after arbitration
  CommandSource   source;          // who won arbitration
  uint16_t        priority;        // numeric priority of winner
  char            reason[48];      // human-readable explanation
  bool            vetoed;          // true if SafetySupervisor/InterlockEngine vetoed
  char            vetoReason[48]; // populated if vetoed
};

// ---------- COMMAND REQUEST (formal input to Arbiter) ----------
struct CommandRequest {
  uint8_t           channelIdx;        // 0..NUM_CHANNELS-1
  CommandSource     source;
  CommandSemantics  semantics;
  bool              targetState;
  const char*       requestId;         // optional UUID (NULL for non-transactional)
  const char*       reason;            // optional human-readable
};

// ---------- COMMAND ARBITER ----------
class CommandArbiter {
public:
  // Single entry point: given the current state of all input sources
  // (manual mode + PIR + schedule + safety), produce the authoritative
  // desired state for the channel.
  //
  // This is the SINGLE authoritative path for computing desired state.
  // RelayEngine::tick() calls this and applies the result via RelayDriver.
  // No other code path may compute desired state directly.
  ArbitrationResult arbitrate(uint8_t channelIdx,
                               uint16_t currentMin, int weekdayIdx);

  // Process an explicit operator/remote command (REST or MQTT ingress).
  // Returns the arbitration result. Caller (REST handler / MQTT handler)
  // applies the result via RelayEngine.
  ArbitrationResult processCommand(const CommandRequest& req);

  // Get the last arbitration result for a channel (for SystemStatus serialization)
  ArbitrationResult getLastResult(uint8_t channelIdx) const;

private:
  ArbitrationResult _lastResult[Core::NUM_CHANNELS] = {};
  bool              _initialized = false;

  // Internal helpers
  bool              _evaluateManual(uint8_t idx, ArbitrationResult& out);
  bool              _evaluateSchedule(uint8_t idx, uint16_t currentMin, int weekdayIdx,
                                      ArbitrationResult& out);
  bool              _evaluatePir(uint8_t idx, ArbitrationResult& out);
  bool              _evaluateSafety(uint8_t idx, ArbitrationResult& out);
  void              _setReason(ArbitrationResult& r, const char* reason);
};

extern CommandArbiter arbiter;

} // namespace Services

#endif // TIMER12_COMMAND_ARBITER_H
