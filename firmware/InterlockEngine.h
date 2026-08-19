// =============================================================================
// InterlockEngine.h — Formal Interlock Engine (audit P1-002)
// Timer Digital Relay v4.3 — Industrial-Grade Hardening Round 2
// -----------------------------------------------------------------------------
// Per ChatGPT audit:
//   "Saya tidak menemukan subsystem formal InterlockEngine dalam struktur
//    firmware yang tersedia."
//
//   "Harus dipisahkan: SET_RELAY dan ACK_SAFETY_ALARM"
//   "Konfigurasi: group: type MUTUAL_EXCLUSION, members [1,2], deadTime 1000ms"
//
// Implements declarative interlock groups:
//   - Mutual exclusion: only one member of a group may be ON at any time
//   - Dead time: minimum time between OFF of one member and ON of another
//   - Audit events: all interlock violations are logged
//
// Groups are configured at runtime (via Channel array or future config file).
// Default: no interlock groups configured (backward compat).
// =============================================================================
#pragma once
#ifndef TIMER12_INTERLOCK_ENGINE_H
#define TIMER12_INTERLOCK_ENGINE_H

#include <Arduino.h>
#include <cstdint>
#include "Config.h"
#include "AlarmRegistry.h"
#include "ErrorCodes.h"

namespace Services {

enum class InterlockType : uint8_t {
  MutualExclusion = 0,  // Only one member ON at a time (forward/reverse, charge/discharge)
  SequenceRequired = 1,  // Members must turn ON in declared order
};

inline const char* interlockTypeStr(InterlockType t) {
  switch (t) {
    case InterlockType::MutualExclusion: return "MUTUAL_EXCLUSION";
    case InterlockType::SequenceRequired: return "SEQUENCE_REQUIRED";
  }
  return "MUTUAL_EXCLUSION";
}

struct InterlockGroup {
  static constexpr uint8_t MAX_MEMBERS = 4;
  InterlockType type;
  uint8_t members[MAX_MEMBERS];   // channel indices (0-based)
  uint8_t memberCount;
  uint16_t deadTimeMs;            // min time between OFF one member and ON another
  char    name[24];               // human-readable group name
  // Runtime state
  uint8_t  activeMember;          // 0xFF if none active; else index into members[]
  unsigned long lastOffMs;         // when last member turned OFF (for dead time check)
};

class InterlockEngine {
public:
  static constexpr uint8_t MAX_GROUPS = 4;

  void begin();

  // Register an interlock group. Returns false if MAX_GROUPS reached.
  bool registerGroup(const char* name, InterlockType type,
                     const uint8_t* members, uint8_t memberCount,
                     uint16_t deadTimeMs);

  // Evaluate whether a desired transition is allowed by interlock constraints.
  // channelIdx: the channel that wants to transition
  // desired: the target state (true=ON, false=OFF)
  // Returns true if transition is ALLOWED, false if BLOCKED by interlock.
  // If blocked, populates reason[48] with explanation.
  bool evaluateTransition(uint8_t channelIdx, bool desired,
                          char reason[48]) const;

  // Called AFTER a transition is applied to update group runtime state.
  void recordTransition(uint8_t channelIdx, bool newState);

  // Clear all groups (factory reset)
  void clear();

  // For SystemStatus serialization
  uint8_t getGroupCount() const { return _groupCount; }
  const InterlockGroup* getGroup(uint8_t idx) const;

private:
  InterlockGroup _groups[MAX_GROUPS] = {};
  uint8_t        _groupCount = 0;

  // Find which group contains the given channel. Returns 0xFF if none.
  uint8_t _findGroup(uint8_t channelIdx) const;
};

extern InterlockEngine interlock;

} // namespace Services

#endif // TIMER12_INTERLOCK_ENGINE_H
