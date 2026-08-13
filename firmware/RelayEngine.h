// =============================================================================
// Services/RelayEngine.h — Unified relay state machine (priority order)
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_RELAY_ENGINE_H
#define TIMER12_SERVICES_RELAY_ENGINE_H

#include <Arduino.h>
#include "Types.h"

namespace Services {

class RelayEngine {
public:
  void tick();                       // Recompute all 12 channels
  void forceRefresh();               // Force recomputation after config change
  void setManual(uint8_t idx, bool on);
  void setMode(uint8_t idx, bool autoMode);
  void toggle(uint8_t idx);

private:
  bool _computeChannel(uint8_t idx, uint16_t currentMin, int weekdayIdx,
                       Core::RelaySource& outSource);
};

extern RelayEngine relayEngine;

} // namespace Services

#endif
