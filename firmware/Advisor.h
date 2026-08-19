// =============================================================================
// AI/Advisor.h — GAS integration (ESP32 → Google Apps Script → Gemini)
// -----------------------------------------------------------------------------
// ESP32 POSTs logs + status summary to GAS Web App every hour.
// GAS calls Gemini API, caches insights for 1 hour.
// PWA GETs insights from same GAS URL every 5 minutes.
// =============================================================================
#pragma once
#ifndef TIMER12_AI_ADVISOR_H
#define TIMER12_AI_ADVISOR_H

#include <Arduino.h>

namespace AI {

class Advisor {
public:
  // Initialize with GAS endpoint URL from Config.h
  void begin(const String& gasUrl = "");

  // Called from main loop() — posts logs to GAS every hour (non-blocking)
  void tick();

  // Check if GAS is configured (URL not empty)
  bool isConfigured() const { return _gasUrl.length() > 0; }

  // Get last sync timestamp (0 = never synced)
  unsigned long getLastSyncMs() const { return _lastSyncMs; }

  // Get last sync result (true = success)
  bool getLastSyncSuccess() const { return _lastSyncSuccess; }

private:
  String _gasUrl;
  unsigned long _lastSyncMs = 0;
  bool _lastSyncSuccess = false;

  bool _postToGAS();
  String _buildPayload();
};

extern Advisor advisor;

} // namespace AI

#endif
