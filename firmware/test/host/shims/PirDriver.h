// =============================================================================
// Stub: PirDriver.h (overrides firmware/PirDriver.h via -I shims)
// =============================================================================
// MqttClient.cpp uses Drivers::pir.testTrigger(idx) in the PIR "test" command.
// The full firmware/PirDriver.h declares the class but the impl is in
// PirDriver.cpp (which uses digitalWrite / GPIO reads). We provide a stub
// that updates Core::pirState[idx] (matches the production side-effect: a
// successful test trigger increments triggerCountToday + sets motionNow).
// =============================================================================
#pragma once
#ifndef HOST_SHIM_PIR_DRIVER_H
#define HOST_SHIM_PIR_DRIVER_H

#include <Arduino.h>
#include "Globals.h"

namespace Drivers {

class PirDriver {
public:
  void begin() {}
  void tick() {}
  bool readDebounced(uint8_t) { return false; }
  bool isMotion(uint8_t idx) const {
    return (idx < Core::NUM_PIR) ? Core::pirState[idx].motionNow : false;
  }
  bool isStuck(uint8_t idx) const {
    return (idx < Core::NUM_PIR) ? Core::pirState[idx].stuckAlerted : false;
  }
  void testTrigger(uint8_t idx) {
    if (idx < Core::NUM_PIR) {
      Core::pirState[idx].triggerCountToday++;
      Core::pirState[idx].motionNow = true;
      Core::pirState[idx].lastMotion = millis();
      Core::pirState[idx].everTriggered = true;
    }
  }
  void resetAll() {}
  void resetDailyCounters() {}
};

extern PirDriver pir;

} // namespace Drivers

#endif // HOST_SHIM_PIR_DRIVER_H
