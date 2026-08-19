// =============================================================================
// Drivers/RtcDriver.h — DS3231 RTC wrapper
// =============================================================================
#pragma once
#ifndef TIMER12_DRIVERS_RTC_H
#define TIMER12_DRIVERS_RTC_H

#include <Arduino.h>
#include <RTClib.h>
#include "Types.h"

namespace Drivers {

class RtcDriver {
public:
  bool begin();
  bool isValid();
  uint32_t getUnixTime();
  void getDateTime(int& y, int& m, int& d, int& h, int& mi, int& s, int& weekday);
  void adjust(int y, int m, int d, int h, int mi, int s);
  void adjust(uint32_t unixTime);
  int getWeekdayIndex();  // 0=Mon ... 6=Sun (matches dayMask convention)
  String formatTime();    // "HH:MM:SS"
  String formatDate();    // "YYYY-MM-DD"

private:
  RTC_DS3231 _rtc;
  bool _initialized = false;
};

extern RtcDriver rtc;

} // namespace Drivers

#endif
