// =============================================================================
// Stub: RelayDriver.h (overrides firmware/RelayDriver.h via -I shims)
// =============================================================================
// MqttClient.cpp uses Drivers::relay.readLogicalState(idx) for GPIO readback
// after a relay command. The full firmware/RelayDriver.h declares the class
// but the impl is in RelayDriver.cpp (which uses digitalWrite/pinMode —
// unavailable on host). We provide a minimal stub that returns the in-RAM
// tracked state (Core::relayState[idx]).
// =============================================================================
#pragma once
#ifndef HOST_SHIM_RELAY_DRIVER_H
#define HOST_SHIM_RELAY_DRIVER_H

#include <Arduino.h>
#include "Globals.h"

namespace Drivers {

class RelayDriver {
public:
  void begin() {}
  void setChannel(uint8_t idx, bool on) {
    if (idx < Core::NUM_CHANNELS) Core::relayState[idx] = on;
  }
  bool getState(uint8_t idx) const {
    return (idx < Core::NUM_CHANNELS) ? Core::relayState[idx] : false;
  }
  void allOff() {
    for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) Core::relayState[i] = false;
  }
  // GPIO readback — in shim, returns the in-RAM state (matches what we wrote).
  // Tests that need to simulate a GPIO mismatch can overwrite Core::relayState[idx]
  // directly before this call.
  bool readLogicalState(uint8_t idx) const {
    return (idx < Core::NUM_CHANNELS) ? Core::relayState[idx] : false;
  }
private:
  bool _state[Core::NUM_CHANNELS] = {false};
};

extern RelayDriver relay;

} // namespace Drivers

#endif // HOST_SHIM_RELAY_DRIVER_H
