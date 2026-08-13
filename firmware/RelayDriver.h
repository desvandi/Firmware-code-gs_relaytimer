// =============================================================================
// Drivers/RelayDriver.h — 12-channel relay control (active-LOW)
// =============================================================================
#pragma once
#ifndef TIMER12_DRIVERS_RELAY_H
#define TIMER12_DRIVERS_RELAY_H

#include <Arduino.h>
#include "Config.h"
#include "Types.h"

namespace Drivers {

class RelayDriver {
public:
  void begin();                              // Boot glitch fix: set level before OUTPUT
  void setChannel(uint8_t idx, bool on);     // Apply state to relay
  bool getState(uint8_t idx) const;
  void allOff();                             // Emergency off (e.g., factory reset)

private:
  bool _state[Core::NUM_CHANNELS] = {false};
};

extern RelayDriver relay;

} // namespace Drivers

#endif
