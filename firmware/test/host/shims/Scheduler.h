// =============================================================================
// Stub: Scheduler.h (overrides firmware/Scheduler.h via -I shims)
// =============================================================================
// MqttClient.cpp includes Scheduler.h but does NOT directly use the
// Services::scheduler singleton. The full firmware/Scheduler.h declares
// the class but impl is in Scheduler.cpp. We provide a minimal stub.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_SCHEDULER_H
#define HOST_SHIM_SCHEDULER_H

#include <Arduino.h>
#include "Types.h"

namespace Services {

class Scheduler {
public:
  bool isScheduleActive(const Core::Schedule&, uint16_t, int) { return false; }
  bool isChannelScheduled(uint8_t, uint16_t, int) { return false; }
  void save(bool /*force*/ = false) {}
};

extern Scheduler scheduler;

} // namespace Services

#endif // HOST_SHIM_SCHEDULER_H
