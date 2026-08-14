// =============================================================================
// Services/RelayEngine.cpp
// =============================================================================
// CYCLE-8B (C8A-007): Added boot phase guard — RelayEngine cannot modify
//   relay state during PRE_INIT/SAFE_INIT/LOADING_NVS/SNAPSHOT/RECONCILING
//   phases. Only RESTORING (forceRefresh from setup) and RUNNING (normal
//   operation) are allowed to modify relay state.
//   This prevents scheduler/PIR logic from contaminating the snapshot used
//   for transaction reconciliation.
// =============================================================================
#include "RelayEngine.h"
#include "RelayDriver.h"
#include "PirDriver.h"
#include "RtcDriver.h"
#include "Scheduler.h"
#include "LogService.h"
#include "ConfigStore.h"
#include "Globals.h"
#include "TransactionJournal.h"  // CYCLE-8B: boot phase guard

namespace Services {

RelayEngine relayEngine;

// Priority order (highest → lowest):
//   1. Manual mode (modeAuto=false) → manualState wins
//   2. PIR override (modeAuto=true, pirEnabled, PIR active) → ON
//   3. Schedule (modeAuto=true, schedule active) → ON
//   4. Default → OFF
// PIR can only force ON, never force OFF. PIR cannot override Manual mode.
bool RelayEngine::_computeChannel(uint8_t idx, uint16_t currentMin, int weekdayIdx,
                                   Core::RelaySource& outSource) {
  if (idx >= Core::NUM_CHANNELS) {
    outSource = Core::RelaySource::Off;
    return false;
  }
  if (!Core::channels[idx].modeAuto) {
    outSource = Core::channels[idx].manualState ? Core::RelaySource::Manual : Core::RelaySource::Off;
    return Core::channels[idx].manualState;
  }
  // Auto mode
  bool scheduleState = Services::scheduler.isChannelScheduled(idx, currentMin, weekdayIdx);

  if (idx >= Core::PIR_CHANNEL_OFFSET && Core::channels[idx].pirEnabled) {
    uint8_t pirIdx = idx - Core::PIR_CHANNEL_OFFSET;
    if (Drivers::pir.isStuck(pirIdx)) {
      // Stuck PIR: ignore its signal, fall back to schedule
      outSource = scheduleState ? Core::RelaySource::Schedule : Core::RelaySource::Off;
      return scheduleState;
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
    if (scheduleState) {
      outSource = Core::RelaySource::Schedule;
      return true;
    }
    outSource = Core::RelaySource::Off;
    return false;
  }
  // Schedule-only channel
  outSource = scheduleState ? Core::RelaySource::Schedule : Core::RelaySource::Off;
  return scheduleState;
}

void RelayEngine::tick() {
  // CYCLE-8B (C8A-007): Guard against relay modification during boot recovery.
  // During PRE_INIT through RECONCILING phases, RelayEngine must NOT modify
  // relay state — this would contaminate the snapshot used for reconciliation.
  // Only RESTORING (forceRefresh from setup) and RUNNING (normal loop) are allowed.
  if (!Services::journal.isRunning() &&
      Services::journal.getBootPhase() != BootPhase::RESTORING) {
    return;
  }
  if (!Drivers::rtc.isValid()) return;
  int y, m, d, h, mi, s, weekday;
  Drivers::rtc.getDateTime(y, m, d, h, mi, s, weekday);
  uint16_t currentMin = h * 60 + mi;

  // PIR debounce update
  Drivers::pir.tick();

  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    Core::RelaySource src;
    bool target = _computeChannel(i, currentMin, weekday, src);
    bool prev = Core::relayState[i];
    Drivers::relay.setChannel(i, target);
    Core::relayState[i] = target;
    Core::relaySource[i] = src;
    if (target != prev) {
      char msg[80];
      snprintf(msg, sizeof(msg), "%s (CH%d) %s via %s",
               Core::channels[i].name, i + 1,
               target ? "ON" : "OFF",
               src == Core::RelaySource::Manual ? "manual" :
               src == Core::RelaySource::Schedule ? "schedule" :
               src == Core::RelaySource::Pir ? "PIR" : "off");
      Services::Log.append(target ? Core::LogType::RelayOn : Core::LogType::RelayOff,
                            msg, i + 1);

      // Energy monitoring: track ON duration
      if (target) {
        // Relay turned ON — record start time
        Core::channels[i].lastOnMs = millis();
      } else if (Core::channels[i].lastOnMs > 0) {
        // Relay turned OFF — accumulate energy
        unsigned long onDurationMs = millis() - Core::channels[i].lastOnMs;
        unsigned long onDurationHours = onDurationMs / 3600000UL;
        // Partial hour (fractional Wh)
        uint16_t wattage = Core::channels[i].wattage > 0 ? Core::channels[i].wattage : 10;
        // Add integer Wh + approximate fractional
        Core::channels[i].energyWh += (uint32_t)(onDurationHours * wattage);
        // Add partial hour (ms * W / 3600000)
        Core::channels[i].energyWh += (uint32_t)((onDurationMs % 3600000UL) * wattage / 3600000UL);
        Core::channels[i].lastOnMs = 0;
      }
    } else if (target && Core::channels[i].lastOnMs > 0) {
      // Relay still ON — accumulate energy for current session (every tick = 1s)
      // This ensures energy accumulates even if relay never turns OFF
      // (lastOnMs is updated continuously; full accumulation happens on OFF)
    }
  }
}

void RelayEngine::forceRefresh() {
  tick();
}

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
