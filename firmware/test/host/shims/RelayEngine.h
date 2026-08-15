// =============================================================================
// Stub: RelayEngine.h (overrides firmware/RelayEngine.h via -I shims)
// =============================================================================
// MqttClient.cpp uses Services::relayEngine.setManual(idx, on),
// .setMode(idx, autoMode), and .forceRefresh(). The full firmware/RelayEngine.h
// declares the class but the impl is in RelayEngine.cpp (which pulls in
// Scheduler, etc). We provide a minimal stub that updates Core::relayState /
// Core::relaySource / Core::channels[idx].modeAuto directly.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_RELAY_ENGINE_H
#define HOST_SHIM_RELAY_ENGINE_H

#include <Arduino.h>
#include "Globals.h"

namespace Services {

class RelayEngine {
public:
  void tick() {}
  void forceRefresh() {}
  void setManual(uint8_t idx, bool on) {
    if (idx < Core::NUM_CHANNELS) {
      Core::relayState[idx] = on;
      Core::relaySource[idx] = on ? Core::RelaySource::Manual : Core::RelaySource::Off;
    }
  }
  void setMode(uint8_t idx, bool autoMode) {
    if (idx < Core::NUM_CHANNELS) Core::channels[idx].modeAuto = autoMode;
  }
  void toggle(uint8_t idx) {
    if (idx < Core::NUM_CHANNELS) Core::relayState[idx] = !Core::relayState[idx];
  }
};

extern RelayEngine relayEngine;

} // namespace Services

#endif // HOST_SHIM_RELAY_ENGINE_H
