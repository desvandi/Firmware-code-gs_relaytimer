// =============================================================================
// Stub: Json.h (overrides firmware/Json.h via -I shims)
// =============================================================================
// MqttClient.cpp uses Utils::parseMinutes (in schedule upsert validation)
// and Utils::isValidDate (in time-set validation). The full firmware/Json.h
// declares these but the impl is in Json.cpp (which uses ArduinoJson). We
// provide inline stubs sufficient for the parsing MqttClient.cpp does.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_JSON_H
#define HOST_SHIM_JSON_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Crc.h"

namespace Utils {

// Parse "HH:MM" → minutes since midnight (matches Json.cpp impl).
inline bool parseMinutes(const char* str, uint16_t& minutes) {
  if (!str) return false;
  size_t len = strlen(str);
  if (len != 5) return false;
  if (str[2] != ':') return false;
  for (int i : {0, 1, 3, 4}) {
    if (str[i] < '0' || str[i] > '9') return false;
  }
  int h = (str[0] - '0') * 10 + (str[1] - '0');
  int m = (str[3] - '0') * 10 + (str[4] - '0');
  if (h > 23 || m > 59) return false;
  minutes = (uint16_t)(h * 60 + m);
  return true;
}

// Basic date validation (matches Json.cpp impl).
inline bool isValidDate(int y, int m, int d) {
  if (y < 2000 || y > 2100) return false;
  if (m < 1 || m > 12) return false;
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxDay = days[m - 1];
  if (m == 2) {
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    if (leap) maxDay = 29;
  }
  return d >= 1 && d <= maxDay;
}

// Stubs for helpers declared in Json.h but not used by MqttClient.cpp.
inline uint32_t computeDocCRC(JsonDocument&) { return 0; }
inline void appendCRC(JsonDocument&) {}
inline bool isPasswordStrong(const String&) { return true; }
inline String sanitizeForLog(const String&, size_t = 80) { return ""; }

} // namespace Utils

#endif // HOST_SHIM_JSON_H
