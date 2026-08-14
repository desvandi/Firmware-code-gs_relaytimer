// =============================================================================
// Services/LogService.cpp
// =============================================================================
#include "LogService.h"
#include "FileSystem.h"
#include "Config.h"
#include "Globals.h"
#include "RtcDriver.h"
#include "MqttClient.h"
#include <ArduinoJson.h>
#include <vector>

namespace Services {

LogServiceClass Log;

static const char* LOG_TYPE_NAMES[] = {
  "relay_on", "relay_off", "pir_trigger", "login", "logout",
  "error", "restart", "ota", "config_change", "factory_reset",
  "time_sync", "auth_fail"
};

void LogServiceClass::begin() {
  // Load next ID from existing log (best-effort)
  // For simplicity, start at 1 — IDs will increment monotonically per boot.
  _nextId = 1;
}

void LogServiceClass::_rotateIfNeeded() {
  if (!Storage::fs.exists(Core::PATH_ACTIVITY_LOG)) return;
  File f = Storage::fs.open(Core::PATH_ACTIVITY_LOG, "r");
  if (!f) return;
  size_t sz = f.size();
  f.close();
  if (sz > Core::MAX_AUDIT_LOG_SIZE * 4) {  // ~32 KB
    Storage::fs.remove(Core::PATH_ACTIVITY_LOG_OLD);
    Storage::fs.rename(Core::PATH_ACTIVITY_LOG, Core::PATH_ACTIVITY_LOG_OLD);
  }
}

void LogServiceClass::_persistActivity(const Core::ActivityLogEntry& e) {
  _rotateIfNeeded();
  File f = Storage::fs.open(Core::PATH_ACTIVITY_LOG, "a");
  if (!f) return;
  // Compact JSON-lines: one entry per line
  StaticJsonDocument<256> doc;
  doc["id"] = e.id;
  doc["ts"] = e.timestamp;
  doc["type"] = LOG_TYPE_NAMES[(uint8_t)e.type];
  doc["ch"] = e.channelId;
  doc["msg"] = e.message;
  serializeJson(doc, f);
  f.println();
  f.close();
}

void LogServiceClass::append(Core::LogType type, const char* msg, int8_t channelId) {
  Core::ActivityLogEntry e;
  e.id = _nextId++;
  e.timestamp = Drivers::rtc.getUnixTime();
  if (e.timestamp == 0) e.timestamp = millis() / 1000;
  e.type = type;
  e.channelId = channelId;
  strncpy(e.message, msg, sizeof(e.message) - 1);
  e.message[sizeof(e.message) - 1] = '\0';
  _persistActivity(e);
  // Also push to MQTT for real-time remote log streaming.
  // CYCLE-7 (fixes F-023): pass the actual monotonic ID instead of letting
  //   MqttClient use millis() which collides on wrap and isn't unique per-boot.
  mqtt.publishLog(type, String(msg), channelId, e.id);
}

void LogServiceClass::append(Core::LogType type, const String& msg, int8_t channelId) {
  append(type, msg.c_str(), channelId);
}

void LogServiceClass::appendAudit(const String& entry) {
  // Audit log is separate plain-text log for forensics
  if (!Storage::fs.exists(Core::PATH_AUDIT_LOG)) {
    File f = Storage::fs.open(Core::PATH_AUDIT_LOG, "w");
    if (f) { f.println("# Audit log created"); f.close(); }
  }
  File f = Storage::fs.open(Core::PATH_AUDIT_LOG, "a");
  if (!f) return;
  if (f.size() > Core::MAX_AUDIT_LOG_SIZE) {
    f.close();
    Storage::fs.remove(Core::PATH_AUDIT_LOG_OLD);
    Storage::fs.rename(Core::PATH_AUDIT_LOG, Core::PATH_AUDIT_LOG_OLD);
    f = Storage::fs.open(Core::PATH_AUDIT_LOG, "w");
    if (!f) return;
  }
  // Prepend timestamp
  String ts = Drivers::rtc.formatTime();
  f.print(ts);
  f.print(" ");
  f.println(entry);
  f.close();
}

String LogServiceClass::getAuditLogText(size_t maxBytes) {
  String out;
  // Try main first, then old
  if (Storage::fs.exists(Core::PATH_AUDIT_LOG)) {
    File f = Storage::fs.open(Core::PATH_AUDIT_LOG, "r");
    if (f) {
      // Read last maxBytes
      if (f.size() > maxBytes) {
        f.seek(f.size() - maxBytes);
      }
      while (f.available()) {
        out += (char)f.read();
      }
      f.close();
    }
  }
  return out;
}

String LogServiceClass::getActivityLogJson(int limit, int filterType, int filterChannel) {
  // Read activity log file backwards, collect up to `limit` entries
  if (!Storage::fs.exists(Core::PATH_ACTIVITY_LOG)) {
    return "{\"logs\":[],\"total\":0}";
  }
  File f = Storage::fs.open(Core::PATH_ACTIVITY_LOG, "r");
  if (!f) return "{\"logs\":[],\"total\":0}";

  // Read entire file into memory (bounded by rotation to ~32KB)
  size_t sz = f.size();
  if (sz > 32768) sz = 32768;
  f.seek(f.size() - sz);
  std::vector<uint8_t> buf(sz + 1);
  f.read(buf.data(), sz);
  buf[sz] = '\0';
  f.close();

  // Parse line-by-line from the end
  DynamicJsonDocument result(8192);
  JsonArray arr = result.createNestedArray("logs");
  int count = 0;
  // Walk backward
  int end = sz;
  while (end > 0 && count < limit) {
    int start = end - 1;
    while (start > 0 && buf[start - 1] != '\n') start--;
    if (start >= end) { end = start - 1; continue; }
    String line((const char*)&buf[start], end - start);
    line.trim();
    if (line.length() > 0) {
      StaticJsonDocument<256> entry;
      DeserializationError err = deserializeJson(entry, line);
      if (!err) {
        const char* type = entry["type"];
        int ch = entry["ch"] | 0;
        bool matches = true;
        if (filterType >= 0 && type) {
          if (strcmp(type, LOG_TYPE_NAMES[filterType]) != 0) matches = false;
        }
        if (filterChannel > 0 && ch != filterChannel) matches = false;
        if (matches) {
          JsonObject o = arr.createNestedObject();
          o["id"] = entry["id"];
          o["timestamp"] = (uint32_t)(entry["ts"] | 0) * 1000ULL;  // ms epoch for PWA
          o["type"] = type;
          o["channelId"] = ch > 0 ? ch : 0;
          o["message"] = entry["msg"];
          count++;
        }
      }
    }
    end = start - 1;
  }
  result["total"] = count;
  String out;
  serializeJson(result, out);
  return out;
}

} // namespace Services
