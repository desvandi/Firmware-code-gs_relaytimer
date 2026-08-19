// =============================================================================
// Drivers/RelayDriver.cpp
// =============================================================================
#include "RelayDriver.h"
#include "Globals.h"

namespace Drivers {

RelayDriver relay;

void RelayDriver::begin() {
  // GPT-AUD-3: Boot Glitch Fix
  // Set GPIO level BEFORE switching to OUTPUT mode to prevent brief glitch.
  // On ESP32, digitalWrite on INPUT pins activates internal pull-up/down.
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    digitalWrite(Core::RELAY_PINS[i], Core::RELAY_OFF);  // pull-up (active-LOW OFF)
    pinMode(Core::RELAY_PINS[i], OUTPUT);
    digitalWrite(Core::RELAY_PINS[i], Core::RELAY_OFF);
    _state[i] = false;
  }
}

void RelayDriver::setChannel(uint8_t idx, bool on) {
  if (idx >= Core::NUM_CHANNELS) return;
  if (_state[idx] == on) return;
  digitalWrite(Core::RELAY_PINS[idx], on ? Core::RELAY_ON : Core::RELAY_OFF);
  _state[idx] = on;
}

bool RelayDriver::getState(uint8_t idx) const {
  if (idx >= Core::NUM_CHANNELS) return false;
  return _state[idx];
}

void RelayDriver::allOff() {
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    setChannel(i, false);
  }
}

} // namespace Drivers
