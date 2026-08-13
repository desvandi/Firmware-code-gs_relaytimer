// =============================================================================
// Drivers/RtcDriver.cpp
// =============================================================================
#include "RtcDriver.h"
#include "Globals.h"
#include "LogService.h"

namespace Drivers {

RtcDriver rtc;

bool RtcDriver::begin() {
  Wire.begin(Core::I2C_SDA, Core::I2C_SCL, Core::I2C_CLOCK);
  if (!_rtc.begin()) {
    Services::Log.append(Core::LogType::Error, "RTC not detected", 0);
    _initialized = false;
    return false;
  }
  if (_rtc.lostPower()) {
    Services::Log.append(Core::LogType::Error, "RTC lost power - time invalid", 0);
    Core::timeValid = false;
  } else {
    DateTime now = _rtc.now();
    if (now.year() >= 2020 && now.year() <= 2099) {
      Core::timeValid = true;
    } else {
      Core::timeValid = false;
      Services::Log.append(Core::LogType::Error, "RTC time out of range", 0);
    }
  }
  _initialized = true;
  return true;
}

bool RtcDriver::isValid() {
  if (!_initialized) return false;
  if (!Core::timeValid) return false;
  DateTime now = _rtc.now();
  if (now.year() < 2020 || now.year() > 2099) {
    Core::timeValid = false;
    return false;
  }
  return true;
}

uint32_t RtcDriver::getUnixTime() {
  if (!_initialized || !Core::timeValid) return 0;
  return _rtc.now().unixtime();
}

void RtcDriver::getDateTime(int& y, int& m, int& d, int& h, int& mi, int& s, int& weekday) {
  if (!_initialized || !Core::timeValid) {
    y = 2000; m = 1; d = 1; h = 0; mi = 0; s = 0; weekday = 0;
    return;
  }
  DateTime now = _rtc.now();
  y = now.year(); m = now.month(); d = now.day();
  h = now.hour(); mi = now.minute(); s = now.second();
  int w = now.dayOfTheWeek();  // 0=Sun ... 6=Sat (RTClib)
  weekday = (w == 0) ? 6 : (w - 1);  // convert to 0=Mon ... 6=Sun
}

void RtcDriver::adjust(int y, int m, int d, int h, int mi, int s) {
  _rtc.adjust(DateTime(y, m, d, h, mi, s));
  Core::timeValid = true;
  char msg[48];
  snprintf(msg, sizeof(msg), "RTC set to %04d-%02d-%02d %02d:%02d:%02d", y, m, d, h, mi, s);
  Services::Log.append(Core::LogType::TimeSync, msg, 0);
}

void RtcDriver::adjust(uint32_t unixTime) {
  _rtc.adjust(DateTime(unixTime));
  Core::timeValid = true;
  char msg[48];
  snprintf(msg, sizeof(msg), "RTC set to unix %lu", (unsigned long)unixTime);
  Services::Log.append(Core::LogType::TimeSync, msg, 0);
}

int RtcDriver::getWeekdayIndex() {
  if (!_initialized || !Core::timeValid) return 0;
  int w = _rtc.now().dayOfTheWeek();
  return (w == 0) ? 6 : (w - 1);
}

String RtcDriver::formatTime() {
  if (!_initialized || !Core::timeValid) return String("--:--:--");
  DateTime now = _rtc.now();
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  return String(buf);
}

String RtcDriver::formatDate() {
  if (!_initialized || !Core::timeValid) return String("----");
  DateTime now = _rtc.now();
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", now.year(), now.month(), now.day());
  return String(buf);
}

} // namespace Drivers
