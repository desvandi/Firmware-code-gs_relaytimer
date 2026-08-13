// =============================================================================
// Services/Scheduler.cpp
// =============================================================================
#include "Scheduler.h"
#include "ConfigStore.h"
#include "Globals.h"

namespace Services {

Scheduler scheduler;

bool Scheduler::isScheduleActive(const Core::Schedule& s, uint16_t currentMin, int weekdayIdx) {
  if (!s.enabled) return false;
  // dayMask: bit0=Mon ... bit6=Sun; 0 means every day
  if (s.dayMask != 0 && !(s.dayMask & (1 << weekdayIdx))) return false;
  uint16_t on = s.onMin;
  uint16_t off = s.offMin;
  if (on == off) return false;
  if (on < off) return (currentMin >= on && currentMin < off);
  // Overnight schedule (on > off): active from on to midnight + midnight to off
  return (currentMin >= on || currentMin < off);
}

bool Scheduler::isChannelScheduled(uint8_t idx, uint16_t currentMin, int weekdayIdx) {
  if (idx >= Core::NUM_CHANNELS) return false;
  for (uint8_t j = 0; j < Core::channels[idx].schedCount; j++) {
    if (isScheduleActive(Core::channels[idx].sched[j], currentMin, weekdayIdx)) {
      return true;
    }
  }
  return false;
}

void Scheduler::save(bool force) {
  Storage::config.saveSchedule(force);
}

} // namespace Services
