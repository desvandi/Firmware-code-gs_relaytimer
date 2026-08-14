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
  bool getState(uint8_t idx) const;          // Returns in-RAM tracked state (what we last wrote)
  void allOff();                             // Emergency off (e.g., factory reset)

  // CYCLE-8A (AUDIT-7-001): Read ACTUAL GPIO output level and convert to logical relay state.
  //   This is DIFFERENT from getState() — getState() returns what we THINK we wrote
  //   (in-RAM cache). readLogicalState() reads the actual GPIO pin register.
  //
  //   Use case: after crash + reboot, the in-RAM cache is empty. We need to know
  //   what the GPIO pin actually is to reconcile with journal PENDING entries.
  //
  //   IMPORTANT CAVEAT (documented in Cycle 8B plan):
  //   - GPIO output state ≠ physical relay contact state.
  //   - A welded relay could have GPIO=ON but contact=OFF (or vice versa).
  //   - Without contact feedback hardware, we CANNOT verify physical state.
  //   - readLogicalState() tells us what we COMMANDED, not what the relay DID.
  //   - For 220V safety, this uncertainty must be reflected in ACK messages.
  bool readLogicalState(uint8_t idx) const;

private:
  bool _state[Core::NUM_CHANNELS] = {false};
};

extern RelayDriver relay;

} // namespace Drivers

#endif
