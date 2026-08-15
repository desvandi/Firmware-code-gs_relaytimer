// =============================================================================
// Stub: RtcDriver.h (overrides firmware/RtcDriver.h via -I shims)
// =============================================================================
// MqttClient.cpp uses Drivers::rtc.getUnixTime(), .getDateTime(y,m,d,h,mi,s,wd),
// and .adjust(y,m,d,h,mi,s). The full firmware/RtcDriver.h includes <RTClib.h>
// (unavailable on host). We provide a minimal stub backed by std::time().
// =============================================================================
#pragma once
#ifndef HOST_SHIM_RTC_DRIVER_H
#define HOST_SHIM_RTC_DRIVER_H

#include <Arduino.h>
#include <ctime>
#include "Types.h"

namespace Drivers {

class RtcDriver {
public:
  bool begin() { _initialized = true; return true; }
  bool isValid() { return _initialized; }
  uint32_t getUnixTime() { return (uint32_t)std::time(nullptr); }
  void getDateTime(int& y, int& m, int& d, int& h, int& mi, int& s, int& weekday) {
    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    y = lt->tm_year + 1900;
    m = lt->tm_mon + 1;
    d = lt->tm_mday;
    h = lt->tm_hour;
    mi = lt->tm_min;
    s = lt->tm_sec;
    weekday = lt->tm_wday;
  }
  void adjust(int, int, int, int, int, int) {}
  void adjust(uint32_t) {}
  int getWeekdayIndex() { return 0; }
  String formatTime() { return "00:00:00"; }
  String formatDate() { return "2024-01-01"; }
private:
  bool _initialized = true;
};

extern RtcDriver rtc;

} // namespace Drivers

#endif // HOST_SHIM_RTC_DRIVER_H
