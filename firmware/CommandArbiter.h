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
enum class CommandSemantics : uint8_t {
  IdempotentState       = 0,
  NonIdempotentAction   = 1,
};

inline const char* semanticsStr(CommandSemantics s) {
  switch (s) {
    case CommandSemantics::IdempotentState:      return "IDEMPOTENT_STATE";
    case CommandSemantics::NonIdempotentAction:  return "NON_IDEMPOTENT_ACTION";
  }
  return "IDEMPOTENT_STATE";
}

// v4.3.2 BLOCKER-05: Explicit whitelist of supported command types.
// Per ChatGPT: "Unknown command: REJECT. Jangan menggunakan blacklist."
// Fail-closed: anything NOT in this enum is rejected.
enum class SupportedCommandType : uint8_t {
  SetRelayState       = 0,   // SET_STATE: ON/OFF — idempotent
  SetMode              = 1,   // SET_MODE: auto/manual — idempotent
  AcknowledgeAlarm    = 2,   // ACK safety alarm — idempotent
  ClearSafetyLockout  = 3,   // CLEAR safety lockout — idempotent
  // PULSE, TOGGLE, START_MOTOR, TRIGGER_CONTACTOR, RESET → NOT supported → REJECT
};
constexpr uint8_t NUM_SUPPORTED_COMMANDS = 4;

inline const char* supportedCommandStr(SupportedCommandType t) {
  switch (t) {
    case SupportedCommandType::SetRelayState:      return "SET_RELAY_STATE";
    case SupportedCommandType::SetMode:              return "SET_MODE";
    case SupportedCommandType::AcknowledgeAlarm:     return "ACK_ALARM";
    case SupportedCommandType::ClearSafetyLockout:   return "CLEAR_ALARM";
  }
  return "UNKNOWN";
}

// v4.3.2 BLOCKER-04: Monotonic command sequence for ordering enforcement.
// Per ChatGPT: "Jangan defer ke protocol v5. Implementasikan minimum
// monotonic command sequence/version sekarang."
// requestId = duplicate identity ("pernah diproses?")
// commandSequence = ordering/relevance ("masih relevan?")

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
  uint8_t           channelIdx;
  CommandSource     source;
  CommandSemantics  semantics;
  SupportedCommandType commandType;  // v4.3.2 BLOCKER-05: whitelist type
  bool              targetState;
  const char*       requestId;
  const char*       reason;
  uint32_t          commandSequence;  // v4.3.2 BLOCKER-04: monotonic ordering
};

// ---------- COMMAND ARBITER ----------
class CommandArbiter {
public:
  ArbitrationResult arbitrate(uint8_t channelIdx,
                               uint16_t currentMin, int weekdayIdx);

  // v4.3.2 BLOCKER-04, BLOCKER-05: processCommand now enforces:
  //   1. Whitelist: commandType must be in SupportedCommandType enum (fail-closed)
  //   2. Stale: commandSequence must be > lastAppliedSequence[channel]
  //   3. Duplicate: requestId checked by TransactionJournal (existing)
  ArbitrationResult processCommand(const CommandRequest& req);

  ArbitrationResult getLastResult(uint8_t channelIdx) const;

  // v4.3.2 BLOCKER-04: query last applied sequence per channel
  uint32_t getLastAppliedSequence(uint8_t channelIdx) const {
    if (channelIdx >= Core::NUM_CHANNELS) return 0;
    return _lastAppliedSeq[channelIdx];
  }

private:
  ArbitrationResult _lastResult[Core::NUM_CHANNELS] = {};
  bool              _initialized = false;
  // v4.3.2 BLOCKER-04: per-channel monotonic command sequence
  uint32_t          _lastAppliedSeq[Core::NUM_CHANNELS] = {};

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
