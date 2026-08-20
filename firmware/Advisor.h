// =============================================================================
// AI/Advisor.h — GAS integration (ESP32 → Google Apps Script → Gemini)
// -----------------------------------------------------------------------------
// Phase B: Added fetchInsights() — authenticated GET to GAS for /api/insights.
// Canonical HMAC contract (must match code.gs/Code.gs):
//   POST: "POST\n" + timestamp + "\n" + nonce + "\n" + deviceId + "\n" + body
//   GET:  "GET\n"  + timestamp + "\n" + nonce + "\n" + deviceId + "\n" + ""
// =============================================================================
#pragma once
#ifndef TIMER12_AI_ADVISOR_H
#define TIMER12_AI_ADVISOR_H

#include <Arduino.h>

namespace AI {

class Advisor {
public:
  void begin(const String& gasUrl = "");
  void tick();

  // Phase B: Fetch insights from GAS via authenticated GET.
  String fetchInsights();

  bool isConfigured() const { return _gasUrl.length() > 0; }
  unsigned long getLastSyncMs() const { return _lastSyncMs; }
  bool getLastSyncSuccess() const { return _lastSyncSuccess; }

private:
  String _gasUrl;
  unsigned long _lastSyncMs = 0;
  bool _lastSyncSuccess = false;

  bool _postToGAS();
  String _buildPayload();
  String _buildAuthenticatedUrl(const String& method, const String& body);
};

extern Advisor advisor;

} // namespace AI

#endif
