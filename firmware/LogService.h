// =============================================================================
// Services/LogService.h — Activity log + audit log persistence
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_LOG_H
#define TIMER12_SERVICES_LOG_H

#include <Arduino.h>
#include "Types.h"

namespace Services {

class LogServiceClass {
public:
  void begin();                                          // Load from file, init counters
  void append(Core::LogType type, const char* msg, int8_t channelId);
  void append(Core::LogType type, const String& msg, int8_t channelId);
  void appendAudit(const String& entry);                 // Plain-text audit log
  String getAuditLogText(size_t maxBytes = 8192);        // For /api/audit_log
  String getActivityLogJson(int limit = 200, int filterType = -1, int filterChannel = 0);

private:
  uint32_t _nextId = 1;
  void _persistActivity(const Core::ActivityLogEntry& e);
  void _rotateIfNeeded();
};

extern LogServiceClass Log;

} // namespace Services

// Convenience C-style functions (used by Drivers/PirDriver.cpp via Services::Log.append)
// Already covered by namespace above.

#endif
