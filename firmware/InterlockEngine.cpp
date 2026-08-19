// =============================================================================
// InterlockEngine.cpp — Formal Interlock Engine implementation (audit P1-002)
// =============================================================================
// Mutual exclusion logic:
//   When channel C wants to turn ON:
//     1. Find group G containing C (if any)
//     2. If G.activeMember != 0xFF and G.activeMember != C:
//        → BLOCKED. Reason: "interlock: channel X is active in group G"
//     3. If G.lastOffMs > 0 and (now - G.lastOffMs) < G.deadTimeMs:
//        → BLOCKED. Reason: "interlock: dead time not elapsed"
//     4. Otherwise ALLOWED.
//
//   When channel C turns OFF:
//     1. If G.activeMember == C → set G.activeMember = 0xFF, G.lastOffMs = now
//
// All blocks raise alarm RELAY_INTERLOCK_VIOLATION (P1 audit: "Semua konflik
// harus menghasilkan audit event").
// =============================================================================
#include "InterlockEngine.h"
#include <cstring>
#include <cstdio>

namespace Services {

InterlockEngine interlock;

void InterlockEngine::begin() {
  _groupCount = 0;
  for (uint8_t i = 0; i < MAX_GROUPS; i++) {
    _groups[i] = {};
    _groups[i].memberCount = 0;
    _groups[i].activeMember = 0xFF;
    _groups[i].lastOffMs = 0;
    _groups[i].name[0] = '\0';
  }
}

bool InterlockEngine::registerGroup(const char* name, InterlockType type,
                                      const uint8_t* members, uint8_t memberCount,
                                      uint16_t deadTimeMs) {
  if (_groupCount >= MAX_GROUPS) return false;
  if (memberCount == 0 || memberCount > InterlockGroup::MAX_MEMBERS) return false;
  if (!members) return false;
  InterlockGroup& g = _groups[_groupCount];
  g.type = type;
  g.memberCount = memberCount;
  g.deadTimeMs = deadTimeMs;
  for (uint8_t i = 0; i < memberCount; i++) {
    g.members[i] = members[i];
  }
  g.activeMember = 0xFF;
  g.lastOffMs = 0;
  if (name) {
    strncpy(g.name, name, sizeof(g.name) - 1);
    g.name[sizeof(g.name) - 1] = '\0';
  } else {
    g.name[0] = '\0';
  }
  _groupCount++;
  Serial.printf("[INTERLOCK] group '%s' registered: %u members, deadTime=%u ms\n",
                g.name, memberCount, deadTimeMs);
  return true;
}

uint8_t InterlockEngine::_findGroup(uint8_t channelIdx) const {
  for (uint8_t i = 0; i < _groupCount; i++) {
    for (uint8_t m = 0; m < _groups[i].memberCount; m++) {
      if (_groups[i].members[m] == channelIdx) return i;
    }
  }
  return 0xFF;
}

bool InterlockEngine::evaluateTransition(uint8_t channelIdx, bool desired,
                                          char reason[48]) const {
  if (!desired) {
    // OFF transitions are always allowed by interlock (other groups will pick
    // up the dead time when this channel records the transition)
    return true;
  }
  uint8_t gIdx = _findGroup(channelIdx);
  if (gIdx == 0xFF) return true;  // not in any group → allowed
  const InterlockGroup& g = _groups[gIdx];

  // Check dead time
  if (g.lastOffMs > 0) {
    unsigned long now = millis();
    if (now - g.lastOffMs < g.deadTimeMs) {
      if (reason) {
        snprintf(reason, 48, "interlock dead time: %lu ms remaining",
                 (unsigned long)(g.deadTimeMs - (now - g.lastOffMs)));
      }
      alarms.raise(Err::RELAY_INTERLOCK_VIOLATION, AlarmSeverity::Warning,
                   "interlock dead time violated");
      return false;
    }
  }

  // Check mutual exclusion
  if (g.type == InterlockType::MutualExclusion) {
    if (g.activeMember != 0xFF) {
      // Find the actual channel index of the active member
      uint8_t activeChannel = g.members[g.activeMember];
      if (activeChannel != channelIdx) {
        if (reason) {
          snprintf(reason, 48, "interlock: CH%u active in group '%s'",
                   activeChannel + 1, g.name);
        }
        alarms.raise(Err::RELAY_INTERLOCK_VIOLATION, AlarmSeverity::Warning,
                     "mutual exclusion violation");
        return false;
      }
    }
  }
  return true;
}

void InterlockEngine::recordTransition(uint8_t channelIdx, bool newState) {
  uint8_t gIdx = _findGroup(channelIdx);
  if (gIdx == 0xFF) return;
  InterlockGroup& g = _groups[gIdx];

  // Find the member index of this channel
  uint8_t memberIdx = 0xFF;
  for (uint8_t m = 0; m < g.memberCount; m++) {
    if (g.members[m] == channelIdx) { memberIdx = m; break; }
  }
  if (memberIdx == 0xFF) return;

  if (newState) {
    // ON transition — set as active member
    g.activeMember = memberIdx;
  } else {
    // OFF transition — clear active member + record dead time start
    if (g.activeMember == memberIdx) {
      g.activeMember = 0xFF;
      g.lastOffMs = millis();
    }
  }
}

void InterlockEngine::clear() {
  _groupCount = 0;
  for (uint8_t i = 0; i < MAX_GROUPS; i++) {
    _groups[i] = {};
    _groups[i].activeMember = 0xFF;
    _groups[i].lastOffMs = 0;
  }
}

const InterlockGroup* InterlockEngine::getGroup(uint8_t idx) const {
  if (idx >= _groupCount) return nullptr;
  return &_groups[idx];
}

} // namespace Services
