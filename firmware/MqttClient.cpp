// =============================================================================
// MqttClient.cpp — MQTT client for remote internet access
// =============================================================================
#include "MqttClient.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include "RelayEngine.h"
#include "Scheduler.h"
#include "AuthManager.h"
#include "RtcDriver.h"
#include "RelayDriver.h"
#include "PirDriver.h"
#include "PzemDriver.h"
#include "WifiManager.h"
#include "ConfigStore.h"
#include "Json.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <Update.h>

namespace Services {

MqttClient mqtt;

// Log type names for MQTT log publishing
static const char* LOG_TYPE_NAMES[] = {
  "relay_on", "relay_off", "pir_trigger", "login", "logout",
  "error", "restart", "ota", "config_change", "factory_reset",
  "time_sync", "auth_fail"
};

bool MqttClient::begin() {
  // Use TLS (WiFiClientSecure) if broker port is 8883, otherwise plain TCP
  if (Core::MQTT_BROKER_PORT == 8883) {
    _wifiClientSecure.setInsecure();  // Skip cert validation (ESP32 doesn't have CA bundle by default)
    _mqtt.setClient(_wifiClientSecure);
    Serial.println("[MQTT] Using TLS (port 8883)");
  } else {
    _mqtt.setClient(_wifiClient);
    Serial.println("[MQTT] Using plain TCP (no TLS)");
  }
  _mqtt.setServer(Core::MQTT_BROKER_HOST, Core::MQTT_BROKER_PORT);
  _mqtt.setKeepAlive(Core::MQTT_KEEPALIVE_SEC);
  _mqtt.setBufferSize(Core::MQTT_BUFFER_SIZE);
  _mqtt.setCallback([this](char* topic, byte* payload, unsigned int length) {
    _onMessage(topic, payload, length);
  });

  _buildTopics();

  _clientId = "timer12-" + TimerNet::wifi.getMacAddress();
  Serial.printf("MQTT: client ID = %s\n", _clientId.c_str());
  Serial.printf("MQTT: topics: status=%s, command=%s, log=%s\n",
                _topicStatus.c_str(), _topicCommand.c_str(), _topicLog.c_str());

  _initialized = true;
  return _connect();
}

void MqttClient::_buildTopics() {
  String mac = TimerNet::wifi.getMacAddress();
  String pass = TimerNet::wifi.getMqttPassword();
  // Security: include password in topic path.
  // Without knowing the password, attackers can't subscribe or publish.
  // Format: timer12/<mac>/<password>/<subtopic>
  String base = "timer12/" + mac + "/" + pass;
  _topicStatus = base + "/status";
  _topicCommand = base + "/command";
  _topicLog = base + "/log";
  _topicOnline = base + "/online";
  _topicOta = base + "/ota";
  _topicAck = base + "/ack";
}

bool MqttClient::_connect() {
  if (!_initialized) return false;
  if (!TimerNet::wifi.isConnected()) return false;

  Serial.printf("MQTT: connecting to %s:%d...\n",
                Core::MQTT_BROKER_HOST, Core::MQTT_BROKER_PORT);

  // Build connect params — support both public (no auth) and self-hosted (with auth)
  const char* mqttUser = (strlen(Core::MQTT_BROKER_USERNAME) > 0) ? Core::MQTT_BROKER_USERNAME : NULL;
  const char* mqttPass = (strlen(Core::MQTT_BROKER_PASSWORD) > 0) ? Core::MQTT_BROKER_PASSWORD : NULL;

  // LWT (Last Will and Testament): publish "0" to online topic on disconnect
  bool connected = _mqtt.connect(
    _clientId.c_str(),
    mqttUser, mqttPass,    // username/password (NULL if public broker)
    _topicOnline.c_str(),  // LWT topic
    1,                     // QoS 1
    true,                  // retain
    "0"                    // LWT message
  );

  if (connected) {
    Serial.println("MQTT: connected!");
    _mqtt.subscribe(_topicCommand.c_str(), 1);  // QoS 1
    _mqtt.subscribe(_topicOta.c_str(), 1);      // OTA commands
    _mqtt.publish(_topicOnline.c_str(), "1", true);  // retain
    Services::Log.append(Core::LogType::Login, "MQTT connected to broker", 0);
    publishStatus();  // publish initial status
    return true;
  }

  Serial.printf("MQTT: connect failed, state=%d\n", _mqtt.state());
  return false;
}

void MqttClient::loop() {
  if (!_initialized) return;
  if (!TimerNet::wifi.isConnected()) return;

  if (!_mqtt.connected()) {
    unsigned long now = millis();
    if (now - _lastReconnectMs < Core::MQTT_RECONNECT_DELAY_MS) return;
    _lastReconnectMs = now;
    _connect();
    return;
  }

  _mqtt.loop();

  // Periodic status publish
  unsigned long now = millis();
  if (now - _lastPublishMs >= Core::MQTT_STATUS_PUBLISH_INTERVAL_MS) {
    _lastPublishMs = now;
    publishStatus();
  }
}

bool MqttClient::isConnected() {
  return _mqtt.connected();
}

// ---------------------------------------------------------------------------
// Publish full SystemStatus JSON to MQTT (same format as REST /api/status)
// ---------------------------------------------------------------------------
void MqttClient::publishStatus() {
  if (!_mqtt.connected()) return;

  DynamicJsonDocument doc(6144);  // Increased for PZEM + schedules + alarms
  JsonObject data = doc.to<JsonObject>();

  data["firmwareVersion"] = Core::FIRMWARE_VERSION;
  data["buildDate"] = Core::BUILD_DATE;
  data["deviceName"] = Core::deviceName;
  data["uptimeSeconds"] = (uint32_t)(millis() / 1000);
  data["currentTime"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
  data["timezone"] = Core::timezone;
  data["wifiRssi"] = TimerNet::wifi.getRssi();
  data["freeHeap"] = ESP.getFreeHeap();
  data["cpuLoadPercent"] = 10;
  data["flashFreePercent"] = 35;
  data["online"] = true;
  data["mqttConnected"] = _mqtt.connected();

  // PZEM-004T v3.0 power monitoring (if sensor available)
  data["pzemAvailable"] = Drivers::pzem.isAvailable();
  if (Drivers::pzem.isAvailable()) {
    // Raw measurements
    data["voltage"] = Drivers::pzem.getVoltage();
    data["current"] = Drivers::pzem.getCurrent();
    data["power"] = Drivers::pzem.getPower();
    data["energy"] = Drivers::pzem.getEnergy();       // kWh total
    data["frequency"] = Drivers::pzem.getFrequency();
    data["powerFactor"] = Drivers::pzem.getPowerFactor();
    data["powerAlarm"] = Drivers::pzem.hasAlarm();

    // Derived calculations
    data["apparentPower"] = Drivers::pzem.getApparentPower();   // VA
    data["reactivePower"] = Drivers::pzem.getReactivePower();   // VAR

    // Daily statistics
    data["energyToday"] = Drivers::pzem.getEnergyToday();       // kWh today
    data["voltageMin"] = Drivers::pzem.getVoltageMin();
    data["voltageMax"] = Drivers::pzem.getVoltageMax();
    data["currentMax"] = Drivers::pzem.getCurrentMax();
    data["powerMax"] = Drivers::pzem.getPowerMax();
    data["powerAvg"] = Drivers::pzem.getPowerAvg();

    // Alarm state
    JsonObject alarms = data.createNestedObject("alarms");
    Drivers::PzemAlarms a = Drivers::pzem.getAlarms();
    alarms["undervoltage"] = a.undervoltage;
    alarms["overvoltage"] = a.overvoltage;
    alarms["overcurrent"] = a.overcurrent;
    alarms["overpower"] = a.overpower;
    alarms["lowPowerFactor"] = a.lowPowerFactor;
  }

  int y, m, d, h, mi, s, weekday;
  Drivers::rtc.getDateTime(y, m, d, h, mi, s, weekday);
  uint16_t currentMin = h * 60 + mi;

  JsonArray chArr = data.createNestedArray("channels");
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    JsonObject ch = chArr.createNestedObject();
    ch["id"] = i + 1;
    ch["name"] = Core::channels[i].name;
    ch["modeAuto"] = Core::channels[i].modeAuto;
    ch["manualState"] = Core::channels[i].manualState;
    ch["pirEnabled"] = Core::channels[i].pirEnabled;
    ch["pirHoldTime"] = Core::channels[i].pirHoldTime;
    ch["state"] = Core::relayState[i];
    const char* srcStr =
      Core::relaySource[i] == Core::RelaySource::Manual ? "manual" :
      Core::relaySource[i] == Core::RelaySource::Schedule ? "schedule" :
      Core::relaySource[i] == Core::RelaySource::Pir ? "pir" : "off";
    ch["source"] = srcStr;
    ch["hasPir"] = (i >= Core::PIR_CHANNEL_OFFSET);
    // Energy monitoring fields
    ch["energyWh"] = Core::channels[i].energyWh;
    ch["wattage"] = Core::channels[i].wattage;
  }

  // Publish all schedules so PWA Scheduler view works in MQTT mode
  JsonArray schedArr = data.createNestedArray("schedules");
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    for (uint8_t j = 0; j < Core::channels[i].schedCount; j++) {
      JsonObject s = schedArr.createNestedObject();
      // ID = (channelId * 10) + scheduleIndex (simple composite ID for MQTT)
      s["id"] = (i + 1) * 10 + j + 1;
      s["channelId"] = i + 1;
      s["onTime"] = Core::channels[i].sched[j].onTime;
      s["offTime"] = Core::channels[i].sched[j].offTime;
      s["dayMask"] = Core::channels[i].sched[j].dayMask;
      s["enabled"] = Core::channels[i].sched[j].enabled;
    }
  }

  JsonArray pirArr = data.createNestedArray("pirs");
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) {
    JsonObject p = pirArr.createNestedObject();
    p["id"] = i + 1;
    p["channelId"] = Core::PIR_CHANNEL_OFFSET + i + 1;
    p["enabled"] = Core::channels[Core::PIR_CHANNEL_OFFSET + i].pirEnabled;
    p["motionNow"] = Core::pirState[i].motionNow;
    p["triggerCountToday"] = Core::pirState[i].triggerCountToday;
    p["stuckDetected"] = Core::pirState[i].stuckAlerted;
    p["holdTime"] = Core::channels[Core::PIR_CHANNEL_OFFSET + i].pirHoldTime;
  }

  JsonObject stats = data.createNestedObject("stats");
  uint8_t onCount = 0;
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) if (Core::relayState[i]) onCount++;
  stats["relaysOn"] = onCount;
  uint32_t pirToday = 0;
  for (uint8_t i = 0; i < Core::NUM_PIR; i++) pirToday += Core::pirState[i].triggerCountToday;
  stats["pirTriggersToday"] = pirToday;
  stats["errorsToday"] = Core::metrics.errorsToday;

  String json;
  serializeJson(doc, json);
  _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)json.c_str(), json.length(), false);  // no retain (too big)
}

// ---------------------------------------------------------------------------
// Publish log entry to MQTT (real-time, for PWA Activity Log view)
// ---------------------------------------------------------------------------
void MqttClient::publishLog(Core::LogType type, const String& message, int8_t channelId) {
  if (!_mqtt.connected()) return;

  StaticJsonDocument<512> doc;
  doc["id"] = (uint32_t)(millis() & 0xFFFFFF);  // pseudo-unique
  doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
  doc["type"] = LOG_TYPE_NAMES[(uint8_t)type];
  doc["channelId"] = channelId > 0 ? channelId : 0;
  doc["message"] = message;

  String json;
  serializeJson(doc, json);
  _mqtt.publish(_topicLog.c_str(), (const uint8_t*)json.c_str(), json.length(), false);
}

// ---------------------------------------------------------------------------
// Publish command ACK — ESP32 confirms command was received and executed
// PWA subscribes to ack topic and waits for confirmation
// ---------------------------------------------------------------------------
void MqttClient::_publishAck(const String& requestId, bool success, const char* message) {
  if (!_mqtt.connected()) return;

  StaticJsonDocument<256> doc;
  doc["requestId"] = requestId;
  doc["success"] = success;
  doc["message"] = message;
  doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;

  String json;
  serializeJson(doc, json);
  _mqtt.publish(_topicAck.c_str(), (const uint8_t*)json.c_str(), json.length(), false);
  Serial.printf("[MQTT ACK] %s: %s\n", requestId.c_str(), success ? "OK" : "FAIL");
}

void MqttClient::publishOnline() {
  if (_mqtt.connected()) {
    _mqtt.publish(_topicOnline.c_str(), "1", true);
  }
}

// ---------------------------------------------------------------------------
// Handle incoming command from PWA
// ---------------------------------------------------------------------------
void MqttClient::_onMessage(char* topic, byte* payload, unsigned int length) {
  String json;
  json.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    json += (char)payload[i];
  }

  String topicStr(topic);
  Serial.printf("MQTT message on %s: %s\n", topic, json.c_str());

  // Route based on topic
  if (topicStr.endsWith("/ota")) {
    _handleOta(json);
  } else {
    _handleCommand(json);
  }
}

void MqttClient::_handleCommand(const String& json) {
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("MQTT: command JSON parse error");
    return;
  }

  const char* type = doc["type"] | "";
  const char* action = doc["action"] | "";
  String requestId = doc["requestId"] | "";

  // Deduplication: check if this requestId was already processed
  if (requestId.length() > 0 && _isDuplicate(requestId)) {
    Serial.printf("[MQTT] Duplicate command ignored: %s\n", requestId.c_str());
    // Re-send ACK (PWA may have missed the first one)
    _publishAck(requestId, true, "Duplicate command (already executed)");
    return;
  }
  if (requestId.length() > 0) _addProcessed(requestId);

  if (strcmp(type, "relay") == 0) {
    int channelId = doc["channelId"] | 0;
    if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
      if (requestId.length() > 0) _publishAck(requestId, false, "Invalid channelId");
      return;
    }
    uint8_t idx = channelId - 1;

    // SET_STATE only — no TOGGLE for idempotency
    if (strcmp(action, "on") == 0) {
      Services::relayEngine.setManual(idx, true);
    } else if (strcmp(action, "off") == 0) {
      Services::relayEngine.setManual(idx, false);
    } else if (strcmp(action, "set_mode") == 0) {
      const char* mode = doc["mode"] | "";
      bool manualState = doc["manualState"] | false;
      if (strcmp(mode, "auto") == 0) {
        Services::relayEngine.setMode(idx, true);
      } else if (strcmp(mode, "manual") == 0) {
        Services::relayEngine.setMode(idx, false);
        Services::relayEngine.setManual(idx, manualState);
      }
    }
    publishStatus();  // immediate status update after command
    // Send ACK to PWA with actual relay state for deterministic UI update
    if (requestId.length() > 0) {
      bool actualState = Core::relayState[idx];
      const char* actualSource =
        Core::relaySource[idx] == Core::RelaySource::Manual ? "manual" :
        Core::relaySource[idx] == Core::RelaySource::Schedule ? "schedule" :
        Core::relaySource[idx] == Core::RelaySource::Pir ? "pir" : "off";

      StaticJsonDocument<256> ackDoc;
      ackDoc["requestId"] = requestId;
      ackDoc["success"] = true;
      ackDoc["message"] = "Relay command executed";
      ackDoc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
      // Include actual state in ACK for deterministic UI update
      JsonObject ackData = ackDoc.createNestedObject("data");
      ackData["channelId"] = channelId;
      ackData["state"] = actualState;
      ackData["source"] = actualSource;
      ackData["modeAuto"] = Core::channels[idx].modeAuto;

      String ackJson;
      serializeJson(ackDoc, ackJson);
      _mqtt.publish(_topicAck.c_str(), (const uint8_t*)ackJson.c_str(), ackJson.length(), false);
      Serial.printf("[MQTT ACK] %s: OK state=%d source=%s\n", requestId.c_str(), actualState, actualSource);
    }
  }
  else if (strcmp(type, "schedule") == 0) {
    int channelId = doc["channelId"] | 0;
    if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
      if (requestId.length() > 0) _publishAck(requestId, false, "Invalid channelId");
      return;
    }

    if (strcmp(action, "upsert") == 0) {
      const char* onTime = doc["onTime"] | "";
      const char* offTime = doc["offTime"] | "";
      uint8_t dayMask = (uint8_t)(doc["dayMask"] | 0) & 0x7F;
      bool enabled = doc["enabled"] | true;
      int id = doc["id"] | 0;

      uint16_t onMin, offMin;
      if (strlen(onTime) == 5 && strlen(offTime) == 5 &&
          Utils::parseMinutes(onTime, onMin) && Utils::parseMinutes(offTime, offMin) &&
          onMin != offMin) {
        uint8_t idx = channelId - 1;

        // Schedule conflict validation: check overlap with existing schedules
        // on the same channel that share at least one day
        bool hasConflict = false;
        String conflictMsg = "";
        auto checkOverlap = [&](uint16_t newOn, uint16_t newOff, uint8_t newDay, int excludeId) {
          for (uint8_t j = 0; j < Core::channels[idx].schedCount; j++) {
            if (excludeId > 0 && (int)(j + 1) == excludeId) continue;
            Core::Schedule& existing = Core::channels[idx].sched[j];
            // Check if days overlap
            bool dayOverlap = (newDay == 0) || (existing.dayMask == 0) ||
                              ((newDay & existing.dayMask) != 0);
            if (!dayOverlap) continue;
            // Check if time ranges overlap
            bool timeOverlap;
            if (newOn <= newOff) {
              if (existing.onMin <= existing.offMin) {
                timeOverlap = (newOn < existing.offMin && newOff > existing.onMin);
              } else {
                // existing is overnight
                timeOverlap = (newOn < existing.offMin) || (newOff > existing.onMin);
              }
            } else {
              // new is overnight
              if (existing.onMin <= existing.offMin) {
                timeOverlap = (existing.onMin < newOff) || (existing.offMin > newOn);
              } else {
                timeOverlap = true;  // both overnight = always overlap
              }
            }
            if (timeOverlap) {
              hasConflict = true;
              conflictMsg = "Conflict with schedule #" + String(j + 1) +
                            " (" + String(existing.onTime) + "-" + String(existing.offTime) + ")";
            }
          }
        };

        int excludeId = (id > 0 && id <= Core::channels[idx].schedCount) ? id : 0;
        checkOverlap(onMin, offMin, dayMask, excludeId);

        if (hasConflict) {
          Services::Log.append(Core::LogType::Error,
            "Schedule conflict on CH" + String(channelId) + ": " + conflictMsg, channelId);
          // Still save — conflict is a warning, not a blocker
        }

        if (id > 0 && id <= Core::channels[idx].schedCount) {
          uint8_t sIdx = id - 1;
          strncpy(Core::channels[idx].sched[sIdx].onTime, onTime, 5);
          Core::channels[idx].sched[sIdx].onTime[5] = '\0';
          strncpy(Core::channels[idx].sched[sIdx].offTime, offTime, 5);
          Core::channels[idx].sched[sIdx].offTime[5] = '\0';
          Core::channels[idx].sched[sIdx].onMin = onMin;
          Core::channels[idx].sched[sIdx].offMin = offMin;
          Core::channels[idx].sched[sIdx].dayMask = dayMask;
          Core::channels[idx].sched[sIdx].enabled = enabled;
        } else if (Core::channels[idx].schedCount < Core::MAX_SCHEDULES) {
          uint8_t sIdx = Core::channels[idx].schedCount;
          strncpy(Core::channels[idx].sched[sIdx].onTime, onTime, 5);
          Core::channels[idx].sched[sIdx].onTime[5] = '\0';
          strncpy(Core::channels[idx].sched[sIdx].offTime, offTime, 5);
          Core::channels[idx].sched[sIdx].offTime[5] = '\0';
          Core::channels[idx].sched[sIdx].onMin = onMin;
          Core::channels[idx].sched[sIdx].offMin = offMin;
          Core::channels[idx].sched[sIdx].dayMask = dayMask;
          Core::channels[idx].sched[sIdx].enabled = enabled;
          Core::channels[idx].schedCount++;
        }
        Storage::config.markDirty();
        Services::relayEngine.forceRefresh();
        Services::Log.append(Core::LogType::ConfigChange,
          "Schedule saved via MQTT for CH" + String(channelId), channelId);
      }
    } else if (strcmp(action, "delete") == 0) {
      int id = doc["id"] | 0;
      // Search and delete (same logic as REST handler)
      for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
        if (id <= (int)Core::channels[i].schedCount) {
          for (uint8_t j = id - 1; j < Core::channels[i].schedCount - 1; j++) {
            Core::channels[i].sched[j] = Core::channels[i].sched[j + 1];
          }
          Core::channels[i].schedCount--;
          Storage::config.markDirty();
          Services::relayEngine.forceRefresh();
          Services::Log.append(Core::LogType::ConfigChange,
            "Schedule deleted via MQTT", 0);
          break;
        }
        id -= Core::channels[i].schedCount;
      }
    }
    publishStatus();
  }
  else if (strcmp(type, "pir") == 0) {
    int id = doc["id"] | 0;
    if (id < 1 || id > (int)Core::NUM_PIR) return;
    uint8_t idx = id - 1;
    uint8_t chIdx = Core::PIR_CHANNEL_OFFSET + idx;

    if (strcmp(action, "config") == 0) {
      if (doc.containsKey("enabled")) {
        Core::channels[chIdx].pirEnabled = doc["enabled"].as<bool>();
      }
      if (doc.containsKey("holdTime")) {
        int ht = doc["holdTime"] | 120;
        if (ht < 5) ht = 5;
        if (ht > 600) ht = 600;
        Core::channels[chIdx].pirHoldTime = (uint16_t)ht;
      }
      Storage::config.markDirty();
      Services::Log.append(Core::LogType::ConfigChange,
        "PIR " + String(id) + " config via MQTT", chIdx + 1);
    } else if (strcmp(action, "test") == 0) {
      if (millis() >= Core::pirStartupTime + Core::PIR_WARMUP_MS) {
        Drivers::pir.testTrigger(idx);
      }
    }
    publishStatus();
  }
  else if (strcmp(type, "channel") == 0) {
    if (strcmp(action, "rename") == 0) {
      int channelId = doc["channelId"] | 0;
      const char* name = doc["name"] | "";
      if (channelId >= 1 && channelId <= Core::NUM_CHANNELS && strlen(name) > 0) {
        uint8_t idx = channelId - 1;
        strncpy(Core::channels[idx].name, name, Core::MAX_NAME_LEN);
        Core::channels[idx].name[Core::MAX_NAME_LEN] = '\0';
        Storage::config.markDirty();
        Services::Log.append(Core::LogType::ConfigChange,
          "CH" + String(channelId) + " renamed via MQTT: " + String(name), channelId);
      }
    }
    publishStatus();
  }
  else if (strcmp(type, "time") == 0) {
    if (strcmp(action, "set") == 0) {
      const char* dt = doc["datetime"] | "";
      int y, m, d, h, mi, s;
      if (sscanf(dt, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &mi, &s) == 6) {
        if (Utils::isValidDate(y, m, d) && h >= 0 && h <= 23 && mi >= 0 && mi <= 59) {
          Drivers::rtc.adjust(y, m, d, h, mi, s);
          Services::Log.append(Core::LogType::TimeSync,
            "RTC set via MQTT", 0);
        }
      }
    }
  }
  else if (strcmp(type, "system") == 0) {
    if (strcmp(action, "reboot") == 0) {
      Services::Log.append(Core::LogType::Restart, "Reboot via MQTT", 0);
      delay(500);
      ESP.restart();
    } else if (strcmp(action, "getStatus") == 0) {
      publishStatus();
    } else if (strcmp(action, "getEnergyStats") == 0) {
      publishStatus();
    } else if (strcmp(action, "resetEnergyStats") == 0) {
      for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
        Core::channels[i].energyWh = 0;
        Core::channels[i].lastOnMs = 0;
      }
      if (Drivers::pzem.isAvailable()) {
        Drivers::pzem.resetEnergy();
      }
      Services::Log.append(Core::LogType::ConfigChange, "Energy stats reset via MQTT", 0);
      publishStatus();
    } else if (strcmp(action, "resetDailyStats") == 0) {
      if (Drivers::pzem.isAvailable()) {
        Drivers::pzem.resetDailyStats();
      }
      Services::Log.append(Core::LogType::ConfigChange, "Daily stats reset via MQTT", 0);
      publishStatus();
    }
  }
  else if (strcmp(type, "config") == 0) {
    // Device config changes via MQTT (timezone, device name, PZEM energy reset)
    if (strcmp(action, "setDevice") == 0) {
      if (doc.containsKey("deviceName")) {
        const char* dn = doc["deviceName"];
        if (dn && strlen(dn) > 0 && strlen(dn) <= 32) {
          strncpy(Core::deviceName, dn, 32);
          Core::deviceName[32] = '\0';
        }
      }
      if (doc.containsKey("timezone")) {
        const char* tz = doc["timezone"];
        if (tz && strlen(tz) < 40) {
          strncpy(Core::timezone, tz, 39);
          Core::timezone[39] = '\0';
        }
      }
      Storage::config.saveDeviceConfig();
      Services::Log.append(Core::LogType::ConfigChange, "Device config updated via MQTT", 0);
      publishStatus();
    }
  }
}

// ---------------------------------------------------------------------------
// Handle OTA command via MQTT
// PWA publishes: {"action":"update","url":"https://github.com/.../firmware.bin","version":"4.1.0"}
// ESP32 downloads binary from URL via HTTP, verifies, installs.
// ---------------------------------------------------------------------------
void MqttClient::_handleOta(const String& json) {
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;

  const char* action = doc["action"] | "";
  if (strcmp(action, "update") != 0) return;

  const char* url = doc["url"] | "";
  const char* version = doc["version"] | "";

  if (strlen(url) == 0) {
    Services::Log.append(Core::LogType::Error, "OTA: missing URL", 0);
    return;
  }

  Services::Log.append(Core::LogType::Ota,
    String("OTA update requested: v") + version, 0);

  // Publish OTA status: started
  String statusJson = "{\"otaStatus\":\"downloading\",\"progress\":0}";
  _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)statusJson.c_str(), statusJson.length(), false);

  // Download and install
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(_wifiClient, url)) {
    Services::Log.append(Core::LogType::Error, "OTA: HTTP begin failed", 0);
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Services::Log.append(Core::LogType::Error,
      "OTA: HTTP error " + String(httpCode), 0);
    http.end();
    return;
  }

  int totalSize = http.getSize();
  Serial.printf("[OTA] Downloading %d bytes...\n", totalSize);

  if (!Update.begin(totalSize > 0 ? totalSize : Core::OTA_MAX_BINARY_SIZE)) {
    Services::Log.append(Core::LogType::Error, "OTA: Update.begin failed", 0);
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[512];
  size_t written = 0;
  size_t lastProgress = 0;

  while (http.connected() && written < (size_t)totalSize) {
    size_t avail = stream->available();
    if (avail) {
      int c = stream->readBytes(buffer, min((size_t)avail, sizeof(buffer)));
      if (c > 0) {
        Update.write(buffer, c);
        written += c;
        // Publish progress every 10%
        size_t progress = totalSize > 0 ? (written * 100 / totalSize) : 0;
        if (progress >= lastProgress + 10) {
          lastProgress = progress;
          String progJson = "{\"otaStatus\":\"downloading\",\"progress\":" + String(progress) + "}";
          _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)progJson.c_str(), progJson.length(), false);
          Serial.printf("[OTA] Progress: %d%%\n", progress);
        }
      }
    }
    esp_task_wdt_reset();
    delay(1);
  }

  if (Update.end(true) && Update.isFinished()) {
    Services::Log.append(Core::LogType::Ota,
      "OTA success: " + String(written) + " bytes", 0);
    String doneJson = "{\"otaStatus\":\"done\",\"progress\":100,\"newVersion\":\"" + String(version) + "\"}";
    _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)doneJson.c_str(), doneJson.length(), false);
    http.end();
    delay(1000);
    ESP.restart();
  } else {
    Services::Log.append(Core::LogType::Error, "OTA: install failed", 0);
    String failJson = "{\"otaStatus\":\"failed\",\"error\":\"install_failed\"}";
    _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)failJson.c_str(), failJson.length(), false);
    http.end();
  }
}

// ---------------------------------------------------------------------------
// Request deduplication — prevent double-execution of retried commands
// Uses a ring buffer of last 16 requestIds
// ---------------------------------------------------------------------------
#define DEDUP_BUFFER_SIZE 16
static String _processedIds[DEDUP_BUFFER_SIZE];
static uint8_t _dedupIdx = 0;

bool MqttClient::_isDuplicate(const String& requestId) {
  for (uint8_t i = 0; i < DEDUP_BUFFER_SIZE; i++) {
    if (_processedIds[i] == requestId) return true;
  }
  return false;
}

void MqttClient::_addProcessed(const String& requestId) {
  _processedIds[_dedupIdx] = requestId;
  _dedupIdx = (_dedupIdx + 1) % DEDUP_BUFFER_SIZE;
}

} // namespace Services
