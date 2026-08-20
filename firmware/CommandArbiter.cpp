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
#include <Preferences.h>  // AUD-FW-CMD-002 FIX: NVS persistence for _lastAppliedSeq

namespace Services {

CommandArbiter arbiter;

// AUD-FW-CMD-002 FIX: NVS persistence for monotonic command sequence.
// Previously _lastAppliedSeq[] was RAM-only — after reboot, all counters reset
// to 0, so any commandSequence > 0 passes the stale check. An attacker (or
// PWA retry logic) could replay stale commands after power-cycling the device.
static constexpr const char* ARBITER_NVS_NAMESPACE = "t12_arbiter";
static constexpr const char* ARBITER_NVS_KEY_SEQ = "last_seq";

void CommandArbiter::begin() {
  _loadSeqFromNVS();
}

void CommandArbiter::_loadSeqFromNVS() {
  Preferences prefs;
  if (!prefs.begin(ARBITER_NVS_NAMESPACE, true)) {
    Serial.println("[Arbiter] NVS open failed (read) — starting with seq=0");
    return;
  }
  size_t needed = prefs.getBytesLength(ARBITER_NVS_KEY_SEQ);
  if (needed == sizeof(_lastAppliedSeq)) {
    prefs.getBytes(ARBITER_NVS_KEY_SEQ, _lastAppliedSeq, sizeof(_lastAppliedSeq));
    Serial.println("[Arbiter] Loaded _lastAppliedSeq from NVS");
    for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
      if (_lastAppliedSeq[i] > 0) {
        Serial.printf("[Arbiter] CH%d lastSeq=%u\n", i + 1, _lastAppliedSeq[i]);
      }
    }
  }
  prefs.end();
}

void CommandArbiter::_persistSeqToNVS() {
  Preferences prefs;
  if (!prefs.begin(ARBITER_NVS_NAMESPACE, false)) {
    Serial.println("[Arbiter] NVS open failed (write) — seq NOT persisted");
    return;
  }
  prefs.putBytes(ARBITER_NVS_KEY_SEQ, _lastAppliedSeq, sizeof(_lastAppliedSeq));
  prefs.end();
}

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

  // v4.3.2 BLOCKER-05: WHITELIST validation (fail-closed).
  // Per ChatGPT: "Unknown command: REJECT. Jangan menggunakan blacklist."
  // Only SET_RELAY_STATE, SET_MODE, ACK_ALARM, CLEAR_ALARM are accepted.
  // PULSE, TOGGLE, START_MOTOR, etc. → REJECT.
  bool isKnownCommand = false;
  switch (req.commandType) {
    case SupportedCommandType::SetRelayState:
    case SupportedCommandType::SetMode:
    case SupportedCommandType::AcknowledgeAlarm:
    case SupportedCommandType::ClearSafetyLockout:
      isKnownCommand = true;
      break;
    default:
      isKnownCommand = false;
      break;
  }
  if (!isKnownCommand) {
    r.vetoed = true;
    setReasonStr(r.vetoReason, sizeof(r.vetoReason),
                 "REJECT_UNKNOWN_COMMAND — not in whitelist");
    setReasonStr(r.reason, sizeof(r.reason),
                 "unknown command type rejected (fail-closed)");
    alarms.raise(Err::CMD_UNKNOWN_ACTION, AlarmSeverity::Warning,
                 "Unknown command type rejected by whitelist");
    return r;
  }

  // v4.3.2 BLOCKER-04: STALE command detection.
  // Per ChatGPT: "100 ON, 101 OFF, arrival 101 then 100 → 100 ditolak sebagai stale"
  // commandSequence must be > lastAppliedSeq for the channel.
  // commandSequence == 0 means "not tracked" (backward compat — skip check).
  if (req.commandSequence > 0) {
    if (req.commandSequence <= _lastAppliedSeq[req.channelIdx]) {
      r.vetoed = true;
      char buf[64];
      snprintf(buf, sizeof(buf),
               "REJECT_STALE — seq=%u <= lastApplied=%u",
               req.commandSequence, _lastAppliedSeq[req.channelIdx]);
      setReasonStr(r.vetoReason, sizeof(r.vetoReason), buf);
      setReasonStr(r.reason, sizeof(r.reason), "stale command rejected");
      alarms.raise(Err::CMD_SEQUENCE_REORDERED, AlarmSeverity::Warning,
                   "Stale/out-of-order command rejected");
      return r;
    }
    // AUD-FW-CMD-003 FIX: Do NOT update _lastAppliedSeq here (before execution).
    // Previously the seq was consumed BEFORE the command ran, so a failed
    // command would still consume the seq — causing a legitimate retry with
    // the same seq to be rejected as STALE. Now the update is moved to AFTER
    // successful execution (in each return path below).
  }

  // Route to appropriate handler based on command type
  if (req.commandType == SupportedCommandType::AcknowledgeAlarm) {
    // ACK safety alarm — does not change relay state
    bool ok = Services::safety.acknowledgeSafetyAlarm(req.channelIdx);
    r.targetState = false;
    r.source = CommandSource::ManualAuth;
    r.priority = sourcePriority(CommandSource::ManualAuth);
    r.vetoed = !ok;
    setReasonStr(r.reason, sizeof(r.reason), ok ? "alarm acknowledged" : "ACK failed");
    // AUD-FW-CMD-003 FIX: update seq AFTER successful execution only
    if (ok && req.commandSequence > 0) {
      _lastAppliedSeq[req.channelIdx] = req.commandSequence;
      _persistSeqToNVS();  // AUD-FW-CMD-002 FIX: persist to NVS
    }
    return r;
  }
  if (req.commandType == SupportedCommandType::ClearSafetyLockout) {
    bool ok = Services::safety.clearSafetyLockout(req.channelIdx);
    r.targetState = false;
    r.source = CommandSource::ManualAuth;
    r.priority = sourcePriority(CommandSource::ManualAuth);
    r.vetoed = !ok;
    setReasonStr(r.reason, sizeof(r.reason), ok ? "lockout cleared" : "CLEAR failed");
    // AUD-FW-CMD-003 FIX: update seq AFTER successful execution only
    if (ok && req.commandSequence > 0) {
      _lastAppliedSeq[req.channelIdx] = req.commandSequence;
      _persistSeqToNVS();  // AUD-FW-CMD-002 FIX: persist to NVS
    }
    return r;
  }

  // SET_RELAY_STATE or SET_MODE — apply as ManualAuth command
  r.targetState = req.targetState;
  r.source = CommandSource::ManualAuth;
  r.priority = sourcePriority(CommandSource::ManualAuth);
  setReasonStr(r.reason, sizeof(r.reason),
               req.reason ? req.reason : "operator command (whitelisted)");

  Core::channels[req.channelIdx].modeAuto = false;
  Core::channels[req.channelIdx].manualState = req.targetState;

  // AUD-FW-CMD-003 FIX: update seq AFTER successful execution
  // (manualState has been set — RelayEngine::tick() will apply it)
  if (req.commandSequence > 0) {
    _lastAppliedSeq[req.channelIdx] = req.commandSequence;
    _persistSeqToNVS();  // AUD-FW-CMD-002 FIX: persist to NVS
  }

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
