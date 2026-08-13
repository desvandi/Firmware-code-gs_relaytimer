// =============================================================================
// Drivers/PirDriver.h — 4x HC-SR501 PIR with debouncing & stuck detection
// =============================================================================
#pragma once
#ifndef TIMER12_DRIVERS_PIR_H
#define TIMER12_DRIVERS_PIR_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"

namespace Drivers {

class PirDriver {
public:
  void begin();
  void tick();                              // Call every loop iteration (non-blocking)
  bool readDebounced(uint8_t idx);          // 3-sample voting, 50ms interval
  bool isMotion(uint8_t idx) const;
  bool isStuck(uint8_t idx) const;
  void testTrigger(uint8_t idx);            // Manual test trigger (mock motion)
  void resetAll();                          // Reset state after import / factory reset
  void resetDailyCounters();

private:
  void checkStuck(uint8_t idx);
};

extern PirDriver pir;

} // namespace Drivers

#endif
