// =============================================================================
// Stub: LogService.h (overrides firmware/LogService.h via -I shims)
// =============================================================================
// MqttClient.cpp uses only LogServiceClass::append(Core::LogType, const String&,
// int8_t). The full firmware/LogService.h declares the class but the impl is in
// LogService.cpp (which pulls in FileSystem, etc). We provide a minimal stub.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_LOG_SERVICE_H
#define HOST_SHIM_LOG_SERVICE_H

#include <Arduino.h>
#include "Types.h"

namespace Services {

class LogServiceClass {
public:
  void begin() {}
  void append(Core::LogType /*type*/, const char* /*msg*/, int8_t /*channelId*/) {}
  void append(Core::LogType /*type*/, const String& /*msg*/, int8_t /*channelId*/) {}
  void appendAudit(const String& /*entry*/) {}
  String getAuditLogText(size_t /*maxBytes*/ = 8192) { return ""; }
  String getActivityLogJson(int /*limit*/ = 200, int /*filterType*/ = -1, int /*filterChannel*/ = 0) {
    return "[]";
  }
};

extern LogServiceClass Log;

} // namespace Services

#endif // HOST_SHIM_LOG_SERVICE_H
