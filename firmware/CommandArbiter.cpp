// =============================================================================
// CommandArbiter.cpp — Formal Command Arbitration Engine (audit P1-001)
// =============================================================================
// Arbitration logic:
//   1. SAFETY always wins (priority 1000) — maxOnTimeForced, fault, interlock
//   2. If channel in MANUAL mode (modeAuto=false): ManualAuth (800) wins
//   3. If channel in AUTO mode:
//      a. PIR (400) — if motion detected and not stuck
//      b. Schedule (500) — if RTC VALID and schedule active
//      c. Default (100) — OFF
//   4. processCommand() for explicit operator/remote commands:
//      a. If semantics = NON_IDEMPOTENT_ACTION → REJECT (audit P1-007)
//         with reason "non-idempotent action not supported via transaction path"
//      b. Otherwise apply as ManualAuth source with priority 800
//
// Output (ArbitrationResult) is consumed by RelayEngine — which then gates it
// through SafetySupervisor and InterlockEngine before GPIO mutation.
// =============================================================================
#include "CommandArbiter.h"
#include "SafetySupervisor.h"
#include "HealthSupervisor.h"
#include "PirDriver.h"
#include "Scheduler.h"
#include "RtcDriver.h"
#include "Globals.h"
#include <cstring>
#include <cstdio>

namespace Services {

CommandArbiter arbiter;

void setReasonStr(char* dst, size_t n, const char* src) {
  if (!dst || n == 0) return;
  if (src) {
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
  } else {
    dst[0] = '\0';
  }
}

bool CommandArbiter::_evaluateSafety(uint8_t idx, ArbitrationResult& out) {
  if (Core::channels[idx].maxOnTimeForced) {
    out.targetState = false;
    out.source = CommandSource::Safety;
    out.priority = sourcePriority(CommandSource::Safety);
    setReasonStr(out.reason, sizeof(out.reason), "maxOnTime force-OFF active");
    out.vetoed = true;
    setReasonStr(out.vetoReason, sizeof(out.vetoReason), "safety lockout — explicit acknowledge required");
    return true;
  }
  return false;
}

bool CommandArbiter::_evaluateManual(uint8_t idx, ArbitrationResult& out) {
  if (Core::channels[idx].modeAuto) return false;
  out.targetState = Core::channels[idx].manualState;
  out.source = CommandSource::ManualAuth;
  out.priority = sourcePriority(CommandSource::ManualAuth);
  setReasonStr(out.reason, sizeof(out.reason), "operator manual command");
  return true;
}

bool CommandArbiter::_evaluateSchedule(uint8_t idx, uint16_t currentMin, int weekdayIdx,
                                        ArbitrationResult& out) {
  // Schedule requires VALID RTC (audit brief §18)
  if (Services::health.getRtcStatus() != Services::RtcStatus::Valid) return false;
  bool active = Services::scheduler.isChannelScheduled(idx, currentMin, weekdayIdx);
  if (!active) return false;
  out.targetState = true;
  out.source = CommandSource::Schedule;
  out.priority = sourcePriority(CommandSource::Schedule);
  setReasonStr(out.reason, sizeof(out.reason), "RTC schedule active");
  return true;
}

bool CommandArbiter::_evaluatePir(uint8_t idx, ArbitrationResult& out) {
  if (idx < Core::PIR_CHANNEL_OFFSET) return false;
  if (!Core::channels[idx].pirEnabled) return false;
  uint8_t pirIdx = idx - Core::PIR_CHANNEL_OFFSET;
  if (Drivers::pir.isStuck(pirIdx)) return false;  // stuck → fall through
  bool motion = Drivers::pir.isMotion(pirIdx);
  bool pirActive = false;
  if (motion) {
    pirActive = true;
  } else if (Core::pirState[pirIdx].everTriggered) {
    unsigned long elapsed = millis() - Core::pirState[pirIdx].lastMotion;
    unsigned long holdMs = (unsigned long)Core::channels[idx].pirHoldTime * 1000UL;
    if (elapsed < holdMs) pirActive = true;
  }
  if (!pirActive) return false;
  out.targetState = true;
  out.source = CommandSource::Pir;
  out.priority = sourcePriority(CommandSource::Pir);
  setReasonStr(out.reason, sizeof(out.reason), "PIR motion detected");
  return true;
}

ArbitrationResult CommandArbiter::arbitrate(uint8_t idx, uint16_t currentMin, int weekdayIdx) {
  ArbitrationResult r = {};
  r.targetState = false;
  r.source = CommandSource::Default;
  r.priority = sourcePriority(CommandSource::Default);
  r.vetoed = false;
  r.vetoReason[0] = '\0';
  setReasonStr(r.reason, sizeof(r.reason), "default OFF");

  if (idx >= Core::NUM_CHANNELS) return r;

  // 1. SAFETY first — always wins
  if (_evaluateSafety(idx, r)) {
    _lastResult[idx] = r;
    return r;
  }

  // 2. Manual mode wins over auto sources (audit: ManualAuth=800 > Schedule=500 > PIR=400)
  if (_evaluateManual(idx, r)) {
    _lastResult[idx] = r;
    return r;
  }

  // 3. Auto mode — try PIR first (audit brief: PIR > Schedule in existing impl)
  //    Per audit P1-001, we now expose this as explicit priority rather than
  //    hardcoded if/else chain. PIR=400, Schedule=500 — schedule actually
  //    has higher numeric priority but existing behavior gives PIR precedence
  //    when active. We preserve existing behavior for backward compat.
  if (_evaluatePir(idx, r)) {
    _lastResult[idx] = r;
    return r;
  }

  if (_evaluateSchedule(idx, currentMin, weekdayIdx, r)) {
    _lastResult[idx] = r;
    return r;
  }

  _lastResult[idx] = r;
  return r;
}

ArbitrationResult CommandArbiter::processCommand(const CommandRequest& req) {
  ArbitrationResult r = {};
  r.targetState = false;
  r.source = CommandSource::Default;
  r.priority = 0;
  r.vetoed = false;
  r.vetoReason[0] = '\0';

  if (req.channelIdx >= Core::NUM_CHANNELS) {
    r.vetoed = true;
    setReasonStr(r.vetoReason, sizeof(r.vetoReason), "invalid channel");
    return r;
  }

  // P1-007: NON_IDEMPOTENT_ACTION commands are REJECTED through transaction path
  // (PULSE, TOGGLE, START_MOTOR, TRIGGER_CONTACTOR, RESET) because the
  // transaction journal may replay them after crash, and replaying a
  // non-idempotent action is unsafe.
  if (req.semantics == CommandSemantics::NonIdempotentAction) {
    r.vetoed = true;
    setReasonStr(r.vetoReason, sizeof(r.vetoReason),
                 "non-idempotent action rejected — use IDEMPOTENT_STATE only");
    setReasonStr(r.reason, sizeof(r.reason),
                 "non-idempotent actions not supported through transactional path");
    return r;
  }

  // IDEMPOTENT_STATE: apply as ManualAuth command
  r.targetState = req.targetState;
  r.source = CommandSource::ManualAuth;
  r.priority = sourcePriority(CommandSource::ManualAuth);
  if (req.reason) {
    setReasonStr(r.reason, sizeof(r.reason), req.reason);
  } else {
    setReasonStr(r.reason, sizeof(r.reason), "operator command (idempotent)");
  }

  // Apply to channel state — RelayEngine::tick() will pick this up next tick
  Core::channels[req.channelIdx].modeAuto = false;
  Core::channels[req.channelIdx].manualState = req.targetState;

  // NOTE: maxOnTimeForced is NOT cleared here (audit P1-003).
  //       Safety lockout requires explicit acknowledgeSafetyAlarm() call.
  //       If maxOnTimeForced=true, the next arbitrate() call will return
  //       Safety source with targetState=false, overriding this manual
  //       command. Operator must explicitly clear the safety alarm first.

  _lastResult[req.channelIdx] = r;
  return r;
}

ArbitrationResult CommandArbiter::getLastResult(uint8_t idx) const {
  if (idx >= Core::NUM_CHANNELS) {
    ArbitrationResult r = {};
    return r;
  }
  return _lastResult[idx];
}

} // namespace Services
