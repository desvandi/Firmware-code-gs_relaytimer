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

// CYCLE-8A (AUDIT-7-001): Read actual GPIO pin register (not in-RAM cache).
//   After crash + reboot, _state[] is reset to false. We need to read the actual
//   GPIO output level to reconcile with journal PENDING entries.
//
//   For active-LOW relay modules:
//     GPIO = LOW  → relay ON  → logical state = true
//     GPIO = HIGH → relay OFF → logical state = false
//
//   Note: digitalRead() on an OUTPUT pin returns the last written value from
//   the GPIO output register. This tells us what the ESP32 commanded, NOT what
//   the physical relay contact actually did (welded/stuck relays are undetectable).
bool RelayDriver::readLogicalState(uint8_t idx) const {
  if (idx >= Core::NUM_CHANNELS) return false;
  int raw = digitalRead(Core::RELAY_PINS[idx]);
  // RELAY_ON = LOW (active-LOW module). If raw == RELAY_ON, relay is commanded ON.
  return (raw == Core::RELAY_ON);
}

void RelayDriver::allOff() {
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    setChannel(i, false);
  }
}

} // namespace Drivers
