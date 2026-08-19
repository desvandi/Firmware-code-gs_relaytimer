// =============================================================================
// Utils/Json.cpp — JSON helpers
// =============================================================================
#include "Json.h"

namespace Utils {

uint32_t computeDocCRC(JsonDocument& doc) {
  JsonObject root = doc.as<JsonObject>();
  if (root.containsKey("_crc")) {
    root.remove("_crc");
  }
  String json;
  serializeJson(doc, json);
  return calculateCRC((const uint8_t*)json.c_str(), json.length());
}

void appendCRC(JsonDocument& doc) {
  uint32_t crc = computeDocCRC(doc);
  doc["_crc"] = crc;
}

bool parseMinutes(const char* str, uint16_t& minutes) {
  if (str == nullptr) return false;
  if (strlen(str) != 5 || str[2] != ':') return false;
  int h, m;
  if (sscanf(str, "%d:%d", &h, &m) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  minutes = (uint16_t)(h * 60 + m);
  return true;
}

bool isValidDate(int y, int m, int d) {
  if (y < 2020 || y > 2099) return false;
  if (m < 1 || m > 12) return false;
  if (d < 1 || d > 31) return false;
  static const uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t maxDay = daysInMonth[m - 1];
  if (m == 2) {
    bool isLeap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (isLeap) maxDay = 29;
  }
  return d <= maxDay;
}

bool isPasswordStrong(const String& pass) {
  if (pass.length() < 8) return false;
  bool hasAlpha = false, hasDigit = false;
  for (char c : pass) {
    if (isAlpha(c)) hasAlpha = true;
    else if (isDigit(c)) hasDigit = true;
  }
  return hasAlpha && hasDigit;
}

String sanitizeForLog(const String& s, size_t maxLen) {
  String out;
  out.reserve(maxLen);
  size_t i = 0;
  for (char c : s) {
    if (i >= maxLen) break;
    if (c == '\n' || c == '\r') continue;
    if (c < 32) continue;
    out += c;
    i++;
  }
  return out;
}

} // namespace Utils
