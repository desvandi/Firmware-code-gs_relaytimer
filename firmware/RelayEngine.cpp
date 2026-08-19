// =============================================================================
// Services/RelayEngine.cpp
// =============================================================================
// v4.2 audit (brief §7-16): RelayEngine now consults SafetySupervisor before
// every GPIO write. Priority order:
//   SAFETY (1000) > MANUAL (800) > SCHEDULE (500) > PIR (400) > DEFAULT OFF
// Per-channel maxOnTime/minOnTime/minOffTime/anti-chatter are enforced here.
// RTC invalid → scheduler inhibited (brief §18).
// Heartbeat recorded to HealthSupervisor on every tick.
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

namespace Services {

RelayEngine relayEngine;

// Priority order (highest → lowest):
//   1. SAFETY: maxOnTime FORCE OFF (always wins, brief §14)
//   2. Manual mode (modeAuto=false) → manualState wins
//   3. PIR override (modeAuto=true, pirEnabled, PIR active) → ON
//   4. Schedule (modeAuto=true, schedule active) → ON
//   5. Default → OFF
// PIR can only force ON, never force OFF. PIR cannot override Manual mode.
// v4.2: Schedule requires valid RTC (brief §18) — if RTC invalid, schedule
// is skipped and the channel falls through to MANUAL/OFF.
bool RelayEngine::_computeChannel(uint8_t idx, uint16_t currentMin, int weekdayIdx,
                                   Core::RelaySource& outSource) {
  if (idx >= Core::NUM_CHANNELS) {
    outSource = Core::RelaySource::Off;
    return false;
  }
  // §13-16: Safety check for maxOnTime — if channel currently ON and exceeded,
  // force OFF (regardless of normal computation)
  if (Core::channels[idx].maxOnTimeForced) {
    outSource = Core::RelaySource::Off;
    return false;
  }
  if (!Core::channels[idx].modeAuto) {
    outSource = Core::channels[idx].manualState ? Core::RelaySource::Manual : Core::RelaySource::Off;
    return Core::channels[idx].manualState;
  }
  // Auto mode — check RTC validity (brief §18)
  bool scheduleActive = false;
  if (Services::health.getRtcStatus() == Services::RtcStatus::Valid) {
    scheduleActive = Services::scheduler.isChannelScheduled(idx, currentMin, weekdayIdx);
  } else {
    // RTC invalid/unsynced — skip schedule execution (brief §18)
    // Fall through to PIR/manual-only mode
  }

  if (idx >= Core::PIR_CHANNEL_OFFSET && Core::channels[idx].pirEnabled) {
    uint8_t pirIdx = idx - Core::PIR_CHANNEL_OFFSET;
    if (Drivers::pir.isStuck(pirIdx)) {
      // Stuck PIR: ignore its signal, fall back to schedule
      outSource = scheduleActive ? Core::RelaySource::Schedule : Core::RelaySource::Off;
      return scheduleActive;
    }
    bool motion = Drivers::pir.isMotion(pirIdx);
    bool pirActive = false;
    if (motion) {
      pirActive = true;
    } else if (Core::pirState[pirIdx].everTriggered) {
      unsigned long elapsed = millis() - Core::pirState[pirIdx].lastMotion;
      unsigned long holdMs = (unsigned long)Core::channels[idx].pirHoldTime * 1000UL;
      if (elapsed < holdMs) pirActive = true;
    }
    if (pirActive) {
      outSource = Core::RelaySource::Pir;
      return true;
    }
    if (scheduleActive) {
      outSource = Core::RelaySource::Schedule;
      return true;
    }
    outSource = Core::RelaySource::Off;
    return false;
  }
  // Schedule-only channel
  outSource = scheduleActive ? Core::RelaySource::Schedule : Core::RelaySource::Off;
  return scheduleActive;
}

void RelayEngine::tick() {
  // §44: record heartbeat for task stall detection
  Services::health.recordHeartbeat(Services::TaskId::RelayEngine);

  // §14: check for maxOnTime exceeded on currently-ON channels (force OFF)
  uint8_t forcedChannel;
  while ((forcedChannel = Services::safety.checkMaxOnTimeExceeded()) != 0xFF) {
    // Force OFF this channel
    Drivers::relay.setChannel(forcedChannel, false);
    Core::relayState[forcedChannel] = false;
    Core::relaySource[forcedChannel] = Core::RelaySource::Off;
    Services::safety.recordTransition(forcedChannel, false);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s (CH%d) FORCE OFF — maxOnTime exceeded",
             Core::channels[forcedChannel].name, forcedChannel + 1);
    Services::Log.append(Core::LogType::Error, msg, forcedChannel + 1);
  }

  // Compute desired state per channel (only if RTC valid — otherwise we
  // can only honor manual overrides; schedules are inhibited per §18)
  uint16_t currentMin = 0;
  int weekday = 0;
  if (Drivers::rtc.isValid()) {
    int y, m, d, h, mi, s;
    Drivers::rtc.getDateTime(y, m, d, h, mi, s, weekday);
    currentMin = h * 60 + mi;
  }

  // PIR debounce update (always runs — PIR is local, doesn't need RTC)
  Drivers::pir.tick();

  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    Core::RelaySource src;
    bool target = _computeChannel(i, currentMin, weekday, src);
    bool prev = Core::relayState[i];

    // §13-16: Safety supervisor gate — may inhibit or force the transition
    Services::SafetyDecision decision =
        Services::safety.evaluateTransition(i, target, prev);
    bool actualTarget = prev;  // default: keep current state
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
        // Keep current state — safety inhibited this transition
        actualTarget = prev;
        break;
    }

    if (actualTarget != prev) {
      Drivers::relay.setChannel(i, actualTarget);
      Core::relayState[i] = actualTarget;
      Core::relaySource[i] = src;
      Services::safety.recordTransition(i, actualTarget);

      char msg[80];
      snprintf(msg, sizeof(msg), "%s (CH%d) %s via %s",
               Core::channels[i].name, i + 1,
               actualTarget ? "ON" : "OFF",
               src == Core::RelaySource::Manual ? "manual" :
               src == Core::RelaySource::Schedule ? "schedule" :
               src == Core::RelaySource::Pir ? "PIR" : "off");
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

void RelayEngine::setManual(uint8_t idx, bool on) {
  if (idx >= Core::NUM_CHANNELS) return;
  // §13: Clear maxOnTimeForced flag when operator manually toggles
  // (assumes operator has acknowledged the alarm)
  Core::channels[idx].maxOnTimeForced = false;
  Services::alarms.clear(Services::Err::RELAY_MAX_ON_TIME);
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
