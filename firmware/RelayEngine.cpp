// =============================================================================
// Services/RelayEngine.cpp
// =============================================================================
// v4.3 audit (ChatGPT targeted remediation):
//   P1-001: RelayEngine now delegates desired-state computation to CommandArbiter
//           instead of having hardcoded precedence logic inline.
//   P1-003: Manual commands do NOT auto-clear maxOnTimeForced. Operator must
//           call SafetySupervisor::acknowledgeSafetyAlarm() explicitly.
//   P1-004: ALL physical GPIO mutation now flows through ONE authoritative
//           actuator path (RelayEngine::applyChannelState). No other code
//           path may call Drivers::relay.setChannel() directly.
//
// Flow:
//   Input sources (manual/schedule/PIR/safety)
//        ↓
//   CommandArbiter::arbitrate() → ArbitrationResult{targetState, source, priority, reason}
//        ↓
//   InterlockEngine::evaluateTransition() → may BLOCK (mutual exclusion, dead time)
//        ↓
//   SafetySupervisor::evaluateTransition() → may INHIBIT (minOnTime, anti-chatter, etc.)
//        ↓
//   RelayEngine::applyChannelState() ← SINGLE authoritative GPIO mutation path
//        ↓
//   RelayDriver::setChannel()
//        ↓
//   Physical GPIO
// =============================================================================
#include "RelayEngine.h"
#include "RelayDriver.h"
#include "PirDriver.h"
#include "RtcDriver.h"
#include "Scheduler.h"
#include "LogService.h"
#include "ConfigStore.h"
#include "Globals.h"
#include "SafetySupervisor.h"
#include "HealthSupervisor.h"
#include "AlarmRegistry.h"
#include "ErrorCodes.h"
#include "CommandArbiter.h"
#include "InterlockEngine.h"
#include <cstdio>

namespace Services {

RelayEngine relayEngine;

// v4.3.1 D-001: SINGLE authoritative GPIO mutation function.
// All paths (maxOnTime force-off, normal transition, boot policy, test-load)
// route through this function. No other code may call
// Drivers::relay.setChannel() except this function + RelayDriver::allOff()
// (which is for factory reset only).
void RelayEngine::applyChannelState(uint8_t idx, bool newState) {
  if (idx >= Core::NUM_CHANNELS) return;
  Drivers::relay.setChannel(idx, newState);
  Core::relayState[idx] = newState;
  // Update physical state tracking (P1-005, P1-014)
  // Without aux contact feedback, physicalState is UNKNOWN — we set
  // confidence to SOFTWARE_ONLY (not VERIFIED). physicalState[] stays
  // false but stateConfidence indicates we don't actually know.
  Core::relayStateConfidence[idx] = Core::StateConfidence::SoftwareOnly;
  Core::relayStateSequence[idx]++;
  Core::relayStateTimestamp[idx] = millis();
  Services::safety.recordTransition(idx, newState);
  Services::interlock.recordTransition(idx, newState);
}

// v4.3.1 D-001: forceChannelState — for test-load / commissioning paths.
// Routes through applyChannelState (unified GPIO path) but skips safety
// evaluation (caller assumes responsibility). Safety lockout still wins.
bool RelayEngine::forceChannelState(uint8_t idx, bool newState) {
  if (idx >= Core::NUM_CHANNELS) return false;
  // Safety lockout always wins — cannot force ON while maxOnTimeForced
  if (newState && Core::channels[idx].maxOnTimeForced) {
    char msg[80];
    snprintf(msg, sizeof(msg), "CH%u forceChannelState(%s) BLOCKED — safety lockout active",
             idx + 1, newState ? "ON" : "OFF");
    Services::Log.append(Core::LogType::Error, msg, idx + 1);
    return false;
  }
  applyChannelState(idx, newState);
  return true;
}

void RelayEngine::tick() {
  // §44: record heartbeat for task stall detection
  Services::health.recordHeartbeat(Services::TaskId::RelayEngine);

  // §14: check for maxOnTime exceeded on currently-ON channels.
  // Per P1-004, this now routes through applyChannelState() (single path)
  // instead of calling Drivers::relay.setChannel() directly.
  uint8_t forcedChannel;
  while ((forcedChannel = Services::safety.checkMaxOnTimeExceeded()) != 0xFF) {
    applyChannelState(forcedChannel, false);
    Core::relaySource[forcedChannel] = Core::RelaySource::Off;
    char msg[80];
    snprintf(msg, sizeof(msg), "%s (CH%d) FORCE OFF — maxOnTime exceeded (safety lockout active, requires explicit acknowledgement)",
             Core::channels[forcedChannel].name, forcedChannel + 1);
    Services::Log.append(Core::LogType::Error, msg, forcedChannel + 1);
  }

  // Compute desired state per channel via CommandArbiter (P1-001)
  uint16_t currentMin = 0;
  int weekday = 0;
  bool rtcValid = Drivers::rtc.isValid();
  if (rtcValid) {
    int y, m, d, h, mi, s;
    Drivers::rtc.getDateTime(y, m, d, h, mi, s, weekday);
    currentMin = h * 60 + mi;
  }

  // PIR debounce update (always runs — PIR is local, doesn't need RTC)
  Drivers::pir.tick();

  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    // 1. CommandArbiter computes desired state + source + priority
    Services::ArbitrationResult ar = Services::arbiter.arbitrate(i, currentMin, weekday);
    bool target = ar.targetState;
    bool prev = Core::relayState[i];

    // 2. InterlockEngine gates the transition (may BLOCK for mutual exclusion / dead time)
    char interlockReason[48];
    if (target && !Services::interlock.evaluateTransition(i, target, interlockReason)) {
      // Interlock blocked — keep current state
      char msg[96];
      snprintf(msg, sizeof(msg), "CH%u transition blocked — %s",
               i + 1, interlockReason);
      Services::Log.append(Core::LogType::Error, msg, i + 1);
      continue;  // skip this channel — no transition applied
    }

    // 3. SafetySupervisor gates the transition (minOnTime, anti-chatter, maxOnTime lockout)
    Services::SafetyDecision decision =
        Services::safety.evaluateTransition(i, target, prev);
    bool actualTarget = prev;  // default: keep current state
    Core::RelaySource src = Core::RelaySource::Off;
    // Map ArbitrationResult.source back to legacy RelaySource for SystemStatus compat
    switch (ar.source) {
      case Services::CommandSource::ManualAuth:    src = Core::RelaySource::Manual; break;
      case Services::CommandSource::Schedule:      src = Core::RelaySource::Schedule; break;
      case Services::CommandSource::Pir:            src = Core::RelaySource::Pir; break;
      case Services::CommandSource::Safety:
      case Services::CommandSource::Emergency:
      case Services::CommandSource::Maintenance:
      case Services::CommandSource::RemoteAuto:
      case Services::CommandSource::Default:        src = Core::RelaySource::Off; break;
    }
    switch (decision) {
      case Services::SafetyDecision::Allow:
        actualTarget = target;
        break;
      case Services::SafetyDecision::ForceOffMaxOn:
        actualTarget = false;
        src = Core::RelaySource::Off;
        break;
      case Services::SafetyDecision::InhibitMinOn:
      case Services::SafetyDecision::InhibitMinOff:
      case Services::SafetyDecision::InhibitChatter:
      case Services::SafetyDecision::RejectBoot:
        actualTarget = prev;  // keep current state
        break;
    }

    if (actualTarget != prev) {
      // v4.3 P1-004: ALL physical mutation goes through applyChannelState()
      applyChannelState(i, actualTarget);
      Core::relaySource[i] = src;

      char msg[80];
      snprintf(msg, sizeof(msg), "%s (CH%d) %s via %s (priority %u)",
               Core::channels[i].name, i + 1,
               actualTarget ? "ON" : "OFF",
               Services::sourceStr(ar.source), ar.priority);
      Services::Log.append(actualTarget ? Core::LogType::RelayOn : Core::LogType::RelayOff,
                            msg, i + 1);

      // Energy monitoring: track ON duration
      if (actualTarget) {
        Core::channels[i].lastOnMs = millis();
      } else if (Core::channels[i].lastOnMs > 0) {
        unsigned long onDurationMs = millis() - Core::channels[i].lastOnMs;
        unsigned long onDurationHours = onDurationMs / 3600000UL;
        uint16_t wattage = Core::channels[i].wattage > 0 ? Core::channels[i].wattage : 10;
        Core::channels[i].energyWh += (uint32_t)(onDurationHours * wattage);
        Core::channels[i].energyWh += (uint32_t)((onDurationMs % 3600000UL) * wattage / 3600000UL);
        Core::channels[i].lastOnMs = 0;
      }
    }
  }
}

void RelayEngine::forceRefresh() {
  tick();
}

// v4.3 P1-003: setManual() NO LONGER clears maxOnTimeForced.
// Operator must call acknowledgeSafetyAlarm() explicitly before issuing
// a manual ON command on a locked-out channel.
// If channel is in safety lockout, the manual command will be applied to
// manualState (so operator's intent is recorded) but the actual relay
// will remain OFF until lockout is explicitly cleared. The next arbitrate()
// call will return Safety source with targetState=false (overriding manual).
void RelayEngine::setManual(uint8_t idx, bool on) {
  if (idx >= Core::NUM_CHANNELS) return;
  Core::channels[idx].modeAuto = false;
  Core::channels[idx].manualState = on;
  Storage::config.markDirty();
  forceRefresh();
}

void RelayEngine::setMode(uint8_t idx, bool autoMode) {
  if (idx >= Core::NUM_CHANNELS) return;
  Core::channels[idx].modeAuto = autoMode;
  Storage::config.markDirty();
  forceRefresh();
}

void RelayEngine::toggle(uint8_t idx) {
  if (idx >= Core::NUM_CHANNELS) return;
  setManual(idx, !Core::channels[idx].manualState);
}

} // namespace Services
