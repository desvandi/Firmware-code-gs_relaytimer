// =============================================================================
// Services/Scheduler.h — Schedule evaluation engine
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_SCHEDULER_H
#define TIMER12_SERVICES_SCHEDULER_H

#include <Arduino.h>
#include "Types.h"

namespace Services {

class Scheduler {
public:
  // Check if a schedule is active at the given minute-of-week
  bool isScheduleActive(const Core::Schedule& s, uint16_t currentMin, int weekdayIdx);

  // Check if any enabled schedule for channel idx is active
  bool isChannelScheduled(uint8_t idx, uint16_t currentMin, int weekdayIdx);

  // Save / load schedule (delegates to Storage)
  void save(bool force = false);
};

extern Scheduler scheduler;

} // namespace Services

#endif
