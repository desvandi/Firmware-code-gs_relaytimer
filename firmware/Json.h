// =============================================================================
// Utils/Json.h — JSON helpers (response wrapper, CRC attachment)
// =============================================================================
#pragma once
#ifndef TIMER12_UTILS_JSON_H
#define TIMER12_UTILS_JSON_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Crc.h"

namespace Utils {

// Compute CRC over a JSON document (excluding the _crc field)
uint32_t computeDocCRC(JsonDocument& doc);

// Attach _crc field to a document
void appendCRC(JsonDocument& doc);

// Parse "HH:MM" -> minutes since midnight; returns false on invalid input
bool parseMinutes(const char* str, uint16_t& minutes);

// Validate date components
bool isValidDate(int y, int m, int d);

// Password strength check: min 8 chars, must contain letter + digit
bool isPasswordStrong(const String& pass);

// Sanitize string for safe inclusion in log/JSON
String sanitizeForLog(const String& s, size_t maxLen = 80);

} // namespace Utils

#endif
