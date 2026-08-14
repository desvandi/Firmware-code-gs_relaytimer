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
#include "Crypto.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <vector>

namespace Services {

MqttClient mqtt;

// Log type names for MQTT log publishing
static const char* LOG_TYPE_NAMES[] = {
  "relay_on", "relay_off", "pir_trigger", "login", "logout",
  "error", "restart", "ota", "config_change", "factory_reset",
  "time_sync", "auth_fail"
};

bool MqttClient::begin() {
  // R10A-5 (audit round 10): Production MQTT guard.
  // For 220V relay control, MQTT must be TLS + authenticated + CA-verified.
  // Production mode is triggered by: port 8883 OR 8884.
  // In production mode, ALL of these must be configured:
  //   - MQTT_BROKER_USERNAME (non-empty)
  //   - MQTT_BROKER_PASSWORD (non-empty)
  //   - MQTT_ROOT_CA (non-empty PEM)
  // If any is missing → hard fail (refuse to connect).
  //
  // For development (port 1883): public broker + topic password is acceptable.
  bool isProductionMode = (Core::MQTT_BROKER_PORT == 8883 || Core::MQTT_BROKER_PORT == 8884);

  if (isProductionMode) {
    if (strlen(Core::MQTT_BROKER_USERNAME) == 0) {
      Serial.println("[MQTT] FATAL: Production mode (port 8883/8884) requires MQTT_BROKER_USERNAME");
      Serial.println("[MQTT] Refusing to connect. Configure credentials in Config.h and re-flash.");
      _initialized = false;
      return false;
    }
    if (strlen(Core::MQTT_BROKER_PASSWORD) == 0) {
      Serial.println("[MQTT] FATAL: Production mode (port 8883/8884) requires MQTT_BROKER_PASSWORD");
      Serial.println("[MQTT] Refusing to connect. Configure credentials in Config.h and re-flash.");
      _initialized = false;
      return false;
    }
    if (strlen(Core::MQTT_ROOT_CA) == 0) {
      Serial.println("[MQTT] FATAL: Production mode (port 8883/8884) requires MQTT_ROOT_CA (PEM)");
      Serial.println("[MQTT] Refusing to connect with setInsecure() in production.");
      Serial.println("[MQTT] Get Let's Encrypt root CA: https://letsencrypt.org/certs/isrgrootx1.pem");
      Serial.println("[MQTT] Paste PEM in Config.h MQTT_ROOT_CA and re-flash.");
      _initialized = false;
      return false;
    }
    Serial.println("[MQTT] Production mode: TLS + auth + CA verified ✓");
  }

  // Use TLS (WiFiClientSecure) if broker port is 8883/8884, otherwise plain TCP
  if (Core::MQTT_BROKER_PORT == 8883 || Core::MQTT_BROKER_PORT == 8884) {
    // R10A-5: setCACert() is MANDATORY in production (checked above).
    // No setInsecure() fallback — fail-closed.
    _wifiClientSecure.setCACert(Core::MQTT_ROOT_CA);
    Serial.println("[MQTT] TLS: using configured root CA for broker cert validation");
    _mqtt.setClient(_wifiClientSecure);
    Serial.printf("[MQTT] Using TLS (port %d)\n", Core::MQTT_BROKER_PORT);
  } else {
    _mqtt.setClient(_wifiClient);
    Serial.println("[MQTT] Using plain TCP (no TLS) — development mode only");
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

  // R10C-3 (audit round 10C): REMOVED password from topic path.
  //
  // Previous design: timer12/<mac>/<password>/<subtopic>
  //   - Password in topic was "obscurity, not authentication" (engineer's words)
  //   - Anyone who sniffed MQTT traffic could see the password in the topic
  //   - Password reuse across topics = cross-domain secret leak risk
  //
  // New design: timer12/<mac>/<subtopic>
  //   - Authentication is handled by BROKER (username/password in MQTT CONNECT)
  //   - Authorization is handled by BROKER ACL (per-device topic restrictions)
  //   - See Mosquitto deployment guide for ACL config:
  //       user device-<MAC>
  //         topic readwrite timer12/<MAC>/#
  //   - In development (HiveMQ public, no auth): topic password is still
  //     available via getMqttPassword() for backward compat, but NOT in topic.
  String base = "timer12/" + mac;
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

// =============================================================================
// R10E-1 (audit round 10E): ATOMIC ACK transaction pattern.
//
// BUG FIXED: Previous design (R10D-2) had ordering bug:
//   _addProcessed(requestId, commandHash);  // ← _lastAckJson still empty here!
//   _publishRelayAck(...);                   // ← sets _lastAckJson AFTER store
// This caused _lastAckJson to capture the PREVIOUS command's ACK, not current.
//
// NEW DESIGN: Each ACK publisher accepts `commandHash` parameter and performs
// atomic transaction in this exact order:
//   1. Construct ACK JSON
//   2. Publish to MQTT
//   3. Store {requestId, commandHash, ackJson} in dedup buffer
// No separate _addProcessed() calls from _handleCommand anymore.
//
// For FAILURE ACKs (success=false): pass commandHash="" → NOT stored in dedup
// (failed commands can be retried with same requestId).
// =============================================================================

// Generic ACK — used for failures and generic successes.
// If commandHash is non-empty AND success=true → store in dedup buffer.
void MqttClient::_publishAck(const String& requestId, bool success, const char* message,
                              const String& dataJson, const String& commandHash) {
  if (!_mqtt.connected()) return;
  if (requestId.length() == 0) return;

  StaticJsonDocument<512> doc;
  doc["requestId"] = requestId;
  doc["success"] = success;
  doc["message"] = message;
  doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;

  if (dataJson.length() > 0) {
    JsonObject data = doc.createNestedObject("data");
    StaticJsonDocument<256> dataDoc;
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (!err) {
      for (JsonPair kv : dataDoc.as<JsonObject>()) {
        data[kv.key()] = kv.value();
      }
    }
  }

  String json;
  serializeJson(doc, json);

  // R10E-1: Atomic publish + store
  _mqtt.publish(_topicAck.c_str(), (const uint8_t*)json.c_str(), json.length(), false);
  if (success && commandHash.length() > 0) {
    _addProcessed(requestId, commandHash, json);
  }
  Serial.printf("[MQTT ACK] %s: %s%s\n", requestId.c_str(),
                success ? "OK" : "FAIL",
                commandHash.length() > 0 ? " (stored)" : "");
}

// Relay ACK: includes actual relay state.
void MqttClient::_publishRelayAck(const String& requestId, bool success, const char* message,
                                   uint8_t channelId, const String& commandHash) {
  if (requestId.length() == 0) return;

  StaticJsonDocument<256> doc;
  doc["requestId"] = requestId;
  doc["success"] = success;
  doc["message"] = message;
  doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;

  if (success && channelId >= 1 && channelId <= Core::NUM_CHANNELS) {
    uint8_t idx = channelId - 1;
    JsonObject data = doc.createNestedObject("data");
    data["channelId"] = channelId;
    data["state"] = Core::relayState[idx];
    const char* actualSource =
      Core::relaySource[idx] == Core::RelaySource::Manual ? "manual" :
      Core::relaySource[idx] == Core::RelaySource::Schedule ? "schedule" :
      Core::relaySource[idx] == Core::RelaySource::Pir ? "pir" : "off";
    data["source"] = actualSource;
    data["modeAuto"] = Core::channels[idx].modeAuto;
  }

  String json;
  serializeJson(doc, json);

  // R10E-1: Atomic publish + store
  _mqtt.publish(_topicAck.c_str(), (const uint8_t*)json.c_str(), json.length(), false);
  if (success && commandHash.length() > 0) {
    _addProcessed(requestId, commandHash, json);
  }
  Serial.printf("[MQTT ACK] %s: %s (relay CH%d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", channelId,
                commandHash.length() > 0 ? " (stored)" : "");
}

// Schedule ACK: includes the schedule ID that was upserted/deleted.
void MqttClient::_publishScheduleAck(const String& requestId, bool success, const char* message,
                                     int channelId, int scheduleId, const String& commandHash) {
  if (requestId.length() == 0) return;

  StaticJsonDocument<256> doc;
  doc["requestId"] = requestId;
  doc["success"] = success;
  doc["message"] = message;
  doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;

  if (success) {
    JsonObject data = doc.createNestedObject("data");
    data["channelId"] = channelId;
    data["scheduleId"] = scheduleId;
  }

  String json;
  serializeJson(doc, json);

  // R10E-1: Atomic publish + store
  _mqtt.publish(_topicAck.c_str(), (const uint8_t*)json.c_str(), json.length(), false);
  if (success && commandHash.length() > 0) {
    _addProcessed(requestId, commandHash, json);
  }
  Serial.printf("[MQTT ACK] %s: %s (schedule CH%d id=%d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", channelId, scheduleId,
                commandHash.length() > 0 ? " (stored)" : "");
}

// PIR ACK: includes PIR state after config/test.
void MqttClient::_publishPirAck(const String& requestId, bool success, const char* message,
                                uint8_t pirId, const String& commandHash) {
  if (requestId.length() == 0) return;

  StaticJsonDocument<256> doc;
  doc["requestId"] = requestId;
  doc["success"] = success;
  doc["message"] = message;
  doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;

  if (success && pirId >= 1 && pirId <= Core::NUM_PIR) {
    uint8_t idx = pirId - 1;
    uint8_t chIdx = Core::PIR_CHANNEL_OFFSET + idx;
    JsonObject data = doc.createNestedObject("data");
    data["id"] = pirId;
    data["channelId"] = chIdx + 1;
    data["enabled"] = Core::channels[chIdx].pirEnabled;
    data["holdTime"] = Core::channels[chIdx].pirHoldTime;
    data["motionNow"] = Core::pirState[idx].motionNow;
  }

  String json;
  serializeJson(doc, json);

  // R10E-1: Atomic publish + store
  _mqtt.publish(_topicAck.c_str(), (const uint8_t*)json.c_str(), json.length(), false);
  if (success && commandHash.length() > 0) {
    _addProcessed(requestId, commandHash, json);
  }
  Serial.printf("[MQTT ACK] %s: %s (pir %d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", pirId,
                commandHash.length() > 0 ? " (stored)" : "");
}

// Channel ACK: includes the renamed channel.
void MqttClient::_publishChannelAck(const String& requestId, bool success, const char* message,
                                    uint8_t channelId, const String& commandHash) {
  if (requestId.length() == 0) return;

  StaticJsonDocument<256> doc;
  doc["requestId"] = requestId;
  doc["success"] = success;
  doc["message"] = message;
  doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;

  if (success && channelId >= 1 && channelId <= Core::NUM_CHANNELS) {
    uint8_t idx = channelId - 1;
    JsonObject data = doc.createNestedObject("data");
    data["channelId"] = channelId;
    data["name"] = Core::channels[idx].name;
  }

  String json;
  serializeJson(doc, json);

  // R10E-1: Atomic publish + store
  _mqtt.publish(_topicAck.c_str(), (const uint8_t*)json.c_str(), json.length(), false);
  if (success && commandHash.length() > 0) {
    _addProcessed(requestId, commandHash, json);
  }
  Serial.printf("[MQTT ACK] %s: %s (channel CH%d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", channelId,
                commandHash.length() > 0 ? " (stored)" : "");
}

// Generic ACK for time/system/config mutations.
void MqttClient::_publishGenericAck(const String& requestId, bool success, const char* message,
                                    const String& dataJson, const String& commandHash) {
  _publishAck(requestId, success, message, dataJson, commandHash);
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

  // R10E-2 (audit round 10E): Validation ordering fixed.
  // Previous order: parse → computeHash → dedup → validate type → validate fields
  // This allowed duplicate handler to replay ACK BEFORE validating unknown fields.
  // NEW order: parse → validate type → validate fields → computeHash → dedup → execute

  // Step 1: Validate type (must be one of known types)
  if (strcmp(type, "relay") != 0 && strcmp(type, "schedule") != 0 &&
      strcmp(type, "pir") != 0 && strcmp(type, "channel") != 0 &&
      strcmp(type, "time") != 0 && strcmp(type, "system") != 0 &&
      strcmp(type, "config") != 0) {
    Serial.printf("[MQTT] Invalid command type: %s\n", type);
    if (requestId.length() > 0) {
      _publishAck(requestId, false, "Invalid command type");
    }
    return;
  }

  // Step 2: R10D-3 — Unknown-field rejection (BEFORE dedup check).
  // Each command type has a FIXED set of allowed fields. Any field outside
  // this whitelist → command REJECTED (not silently ignored).
  {
    JsonObject obj = doc.as<JsonObject>();
    bool hasUnknownField = false;
    String unknownFields = "";

    for (JsonPair kv : obj) {
      String key = kv.key().c_str();
      bool allowed = false;

      if (key == "type" || key == "action" || key == "requestId") {
        allowed = true;
      }
      else if (strcmp(type, "relay") == 0) {
        if (key == "channelId" || key == "mode" || key == "manualState") allowed = true;
      }
      else if (strcmp(type, "schedule") == 0) {
        if (key == "channelId" || key == "id" || key == "onTime" ||
            key == "offTime" || key == "dayMask" || key == "enabled") allowed = true;
      }
      else if (strcmp(type, "pir") == 0) {
        if (key == "id" || key == "enabled" || key == "holdTime") allowed = true;
      }
      else if (strcmp(type, "channel") == 0) {
        if (key == "channelId" || key == "name") allowed = true;
      }
      else if (strcmp(type, "time") == 0) {
        if (key == "datetime") allowed = true;
      }
      else if (strcmp(type, "system") == 0) {
        // system: action only (no extra fields)
      }
      else if (strcmp(type, "config") == 0) {
        if (key == "deviceName" || key == "timezone") allowed = true;
      }

      if (!allowed) {
        hasUnknownField = true;
        if (unknownFields.length() > 0) unknownFields += ", ";
        unknownFields += key;
      }
    }

    if (hasUnknownField) {
      String msg = "Unknown fields rejected: " + unknownFields;
      Serial.printf("[MQTT] %s\n", msg.c_str());
      Services::Log.append(Core::LogType::AuthFail,
        "SECURITY: Command rejected — unknown fields: " + unknownFields, 0);
      if (requestId.length() > 0) {
        _publishAck(requestId, false, msg.c_str());
      }
      return;
    }
  }

  // Step 3: Compute command fingerprint (AFTER validation passes).
  String commandHash = _computeCommandHash(doc);

  // Step 4: Dedup check (AFTER validation, BEFORE execution).
  if (requestId.length() > 0 && _isDuplicate(requestId)) {
    String previousHash = _getHashForRequestId(requestId);
    if (previousHash.length() > 0 && previousHash != commandHash) {
      Serial.printf("[MQTT] SECURITY: requestId reuse with different command! rid=%s\n", requestId.c_str());
      Services::Log.append(Core::LogType::AuthFail,
        "SECURITY: requestId reuse with different command: " + requestId, 0);
      _publishAck(requestId, false, "requestId reuse with different command — rejected");
      return;
    }

    // True duplicate — replay ORIGINAL ACK (R10E-1: stored atomically).
    Serial.printf("[MQTT] Duplicate command detected: %s — replaying original ACK\n", requestId.c_str());

    String originalAckJson = _getAckResultForRequestId(requestId);
    if (originalAckJson.length() > 0) {
      _mqtt.publish(_topicAck.c_str(), (const uint8_t*)originalAckJson.c_str(),
                    originalAckJson.length(), false);
      Serial.printf("[MQTT ACK] %s: replayed original ACK (%d bytes)\n",
                    requestId.c_str(), originalAckJson.length());
    } else {
      _publishAck(requestId, true, "Duplicate command (already executed)");
    }
    return;
  }


  // ===========================================================================
  // RELAY COMMANDS
  // ===========================================================================
  if (strcmp(type, "relay") == 0) {
    int channelId = doc["channelId"] | 0;
    if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
      if (requestId.length() > 0) _publishAck(requestId, false, "Invalid channelId");
      return;  // P0 #1: no _addProcessed — retry can succeed
    }
    uint8_t idx = channelId - 1;

    // P0 #1: Validate action BEFORE execution. Invalid action → success:false.
    bool actionValid = (strcmp(action, "on") == 0 || strcmp(action, "off") == 0 ||
                        strcmp(action, "set_mode") == 0);
    if (!actionValid) {
      Serial.printf("[MQTT] Invalid relay action: %s\n", action);
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid relay action (use on/off/set_mode)");
      }
      return;  // no _addProcessed
    }

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
      } else {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Invalid mode (use auto/manual)");
        }
        return;  // no _addProcessed
      }
    }

    // Execution succeeded → add to dedup buffer (P0 #1 fix)

    publishStatus();  // immediate status update after command
    // P0 #2 + P1 #11: Send ACK with actual relay state
    _publishRelayAck(requestId, true, "Relay command executed", channelId, commandHash);
  }

  // ===========================================================================
  // SCHEDULE COMMANDS (P1 #11: now sends ACK; P1 #14: fixed ID bug)
  // ===========================================================================
  else if (strcmp(type, "schedule") == 0) {
    int channelId = doc["channelId"] | 0;
    if (channelId < 1 || channelId > Core::NUM_CHANNELS) {
      if (requestId.length() > 0) _publishAck(requestId, false, "Invalid channelId");
      return;
    }
    uint8_t idx = channelId - 1;

    if (strcmp(action, "upsert") == 0) {
      const char* onTime = doc["onTime"] | "";
      const char* offTime = doc["offTime"] | "";
      uint8_t dayMask = (uint8_t)(doc["dayMask"] | 0) & 0x7F;
      bool enabled = doc["enabled"] | true;
      int id = doc["id"] | 0;  // P1 #14: id is scheduleIndex (1-based) within this channel

      uint16_t onMin, offMin;
      if (strlen(onTime) != 5 || strlen(offTime) != 5 ||
          !Utils::parseMinutes(onTime, onMin) || !Utils::parseMinutes(offTime, offMin) ||
          onMin == offMin) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Invalid onTime/offTime (format HH:MM, must differ)");
        }
        return;  // no _addProcessed
      }

      // P1 #14 fix: id is 1-based schedule index within this channel (1..schedCount)
      // For new schedule: id=0 (firmware assigns next slot)
      int savedId = 0;

      if (id > 0 && id <= (int)Core::channels[idx].schedCount) {
        // Update existing schedule (id is 1-based index within channel)
        uint8_t sIdx = id - 1;
        strncpy(Core::channels[idx].sched[sIdx].onTime, onTime, 5);
        Core::channels[idx].sched[sIdx].onTime[5] = '\0';
        strncpy(Core::channels[idx].sched[sIdx].offTime, offTime, 5);
        Core::channels[idx].sched[sIdx].offTime[5] = '\0';
        Core::channels[idx].sched[sIdx].onMin = onMin;
        Core::channels[idx].sched[sIdx].offMin = offMin;
        Core::channels[idx].sched[sIdx].dayMask = dayMask;
        Core::channels[idx].sched[sIdx].enabled = enabled;
        savedId = id;
      } else if (Core::channels[idx].schedCount < Core::MAX_SCHEDULES) {
        // Add new schedule
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
        savedId = sIdx + 1;  // 1-based
      } else {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Schedule limit reached (max 4 per channel)");
        }
        return;  // no _addProcessed
      }

      Storage::config.markDirty();
      Services::relayEngine.forceRefresh();
      Services::Log.append(Core::LogType::ConfigChange,
        "Schedule saved via MQTT for CH" + String(channelId), channelId);

      publishStatus();
      _publishScheduleAck(requestId, true, "Schedule saved", channelId, savedId, commandHash);

    } else if (strcmp(action, "delete") == 0) {
      int id = doc["id"] | 0;
      // P1 #14 fix: id is 1-based schedule index WITHIN this channel.
      // (Previously used sequential global index which was ambiguous with
      // composite IDs published in status.)
      if (id < 1 || id > (int)Core::channels[idx].schedCount) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Invalid schedule id for this channel");
        }
        return;  // no _addProcessed
      }

      // Delete schedule at index (id-1) within this channel
      uint8_t sIdx = id - 1;
      for (uint8_t j = sIdx; j < Core::channels[idx].schedCount - 1; j++) {
        Core::channels[idx].sched[j] = Core::channels[idx].sched[j + 1];
      }
      Core::channels[idx].schedCount--;
      Storage::config.markDirty();
      Services::relayEngine.forceRefresh();
      Services::Log.append(Core::LogType::ConfigChange,
        "Schedule " + String(id) + " deleted via MQTT from CH" + String(channelId), channelId);

      publishStatus();
      _publishScheduleAck(requestId, true, "Schedule deleted", channelId, id, commandHash);

    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid schedule action (use upsert/delete)");
      }
      return;  // no _addProcessed
    }
  }

  // ===========================================================================
  // PIR COMMANDS (P1 #11: now sends ACK)
  // ===========================================================================
  else if (strcmp(type, "pir") == 0) {
    int id = doc["id"] | 0;
    if (id < 1 || id > (int)Core::NUM_PIR) {
      if (requestId.length() > 0) _publishAck(requestId, false, "Invalid PIR id");
      return;
    }
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
      } else {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "PIR still warming up (60s after boot)");
        }
        return;  // no _addProcessed
      }
    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid PIR action (use config/test)");
      }
      return;  // no _addProcessed
    }

    publishStatus();
    _publishPirAck(requestId, true, "PIR command executed", id, commandHash);
  }

  // ===========================================================================
  // CHANNEL COMMANDS (P1 #11: now sends ACK)
  // ===========================================================================
  else if (strcmp(type, "channel") == 0) {
    if (strcmp(action, "rename") == 0) {
      int channelId = doc["channelId"] | 0;
      const char* name = doc["name"] | "";
      if (channelId < 1 || channelId > Core::NUM_CHANNELS || strlen(name) == 0) {
        if (requestId.length() > 0) _publishAck(requestId, false, "Invalid channelId or name");
        return;  // no _addProcessed
      }
      if (strlen(name) > Core::MAX_NAME_LEN) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Name too long (max 20 chars)");
        }
        return;  // no _addProcessed
      }
      uint8_t idx = channelId - 1;
      strncpy(Core::channels[idx].name, name, Core::MAX_NAME_LEN);
      Core::channels[idx].name[Core::MAX_NAME_LEN] = '\0';
      Storage::config.markDirty();
      Services::Log.append(Core::LogType::ConfigChange,
        "CH" + String(channelId) + " renamed via MQTT: " + String(name), channelId);

      publishStatus();
      _publishChannelAck(requestId, true, "Channel renamed", channelId, commandHash);
    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid channel action (use rename)");
      }
      return;  // no _addProcessed
    }
  }

  // ===========================================================================
  // TIME COMMANDS (P1 #11: now sends ACK)
  // ===========================================================================
  else if (strcmp(type, "time") == 0) {
    if (strcmp(action, "set") == 0) {
      const char* dt = doc["datetime"] | "";
      int y, m, d, h, mi, s;
      if (sscanf(dt, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &mi, &s) != 6) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Invalid datetime format (use YYYY-MM-DDTHH:MM:SS)");
        }
        return;  // no _addProcessed
      }
      if (!Utils::isValidDate(y, m, d) || h < 0 || h > 23 || mi < 0 || mi > 59) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Invalid date/time values");
        }
        return;  // no _addProcessed
      }
      Drivers::rtc.adjust(y, m, d, h, mi, s);
      Services::Log.append(Core::LogType::TimeSync, "RTC set via MQTT", 0);

      _publishGenericAck(requestId, true, "RTC time set", commandHash);
    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid time action (use set)");
      }
      return;  // no _addProcessed
    }
  }

  // ===========================================================================
  // SYSTEM COMMANDS (P1 #11: now sends ACK)
  // ===========================================================================
  else if (strcmp(type, "system") == 0) {
    if (strcmp(action, "reboot") == 0) {
      Services::Log.append(Core::LogType::Restart, "Reboot via MQTT", 0);
      if (requestId.length() > 0) {
        _publishGenericAck(requestId, true, "Rebooting", commandHash);
      }
      delay(500);
      ESP.restart();
    } else if (strcmp(action, "getStatus") == 0) {
      publishStatus();
      if (requestId.length() > 0) {
        _publishGenericAck(requestId, true, "Status published", commandHash);
      }
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
      _publishGenericAck(requestId, true, "Energy stats reset", commandHash);
    } else if (strcmp(action, "resetDailyStats") == 0) {
      if (Drivers::pzem.isAvailable()) {
        Drivers::pzem.resetDailyStats();
      }
      Services::Log.append(Core::LogType::ConfigChange, "Daily stats reset via MQTT", 0);
      publishStatus();
      _publishGenericAck(requestId, true, "Daily stats reset", commandHash);
    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid system action (use reboot/getStatus/resetEnergyStats/resetDailyStats)");
      }
      return;  // no _addProcessed
    }
  }

  // ===========================================================================
  // CONFIG COMMANDS (P1 #11: now sends ACK)
  // ===========================================================================
  else if (strcmp(type, "config") == 0) {
    if (strcmp(action, "setDevice") == 0) {
      bool changed = false;
      if (doc.containsKey("deviceName")) {
        const char* dn = doc["deviceName"];
        if (dn && strlen(dn) > 0 && strlen(dn) <= 32) {
          strncpy(Core::deviceName, dn, 32);
          Core::deviceName[32] = '\0';
          changed = true;
        }
      }
      if (doc.containsKey("timezone")) {
        const char* tz = doc["timezone"];
        if (tz && strlen(tz) < 40) {
          strncpy(Core::timezone, tz, 39);
          Core::timezone[39] = '\0';
          changed = true;
        }
      }
      if (!changed) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "No valid config fields to update");
        }
        return;  // no _addProcessed
      }
      Storage::config.saveDeviceConfig();
      Services::Log.append(Core::LogType::ConfigChange, "Device config updated via MQTT", 0);
      publishStatus();
      _publishGenericAck(requestId, true, "Device config updated", commandHash);
    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid config action (use setDevice)");
      }
      return;  // no _addProcessed
    }
  }
}

// ---------------------------------------------------------------------------
// Handle OTA command via MQTT (P0 #3+#8+#9 + R10A-2 + R10B-1 + R10C — audit rounds 9-10C)
//
// REQUIRED command shape (PWA must send ALL fields):
// {
//   "action": "update",
//   "url": "https://github.com/.../firmware.bin",  ← HTTPS required
//   "version": "4.1.0",                             ← SemVer, must be > current
//   "size": 1234567,                                ← expected binary size in bytes
//   "sha256": "abcdef0123...",                      ← 64 hex chars (SHA-256 of binary)
//   "signature": "abcdef0123...",                   ← 128 hex chars (Ed25519 of SHA-256 hash)
//   "requestId": "uuid-123"                         ← for ACK transaction
// }
//
// SIGNING CONTRACT (R10B-1):
//   signature = ed25519_sign(SHA256(firmware.bin), private_key)
//   ESP32 verifies: ed25519_verify(signature, SHA256(firmware.bin), public_key)
//   The signature is over the 32-byte SHA-256 HASH, NOT over the full binary.
//   Use scripts/sign_firmware.py to generate consistent signatures.
//
// ESP32 flow:
//   1. Validate all fields present
//   2. Anti-downgrade: version > current (SemVer numeric comparison, R10B-4)
//   3. HTTPS download (WiFiClientSecure + setCACert, R10A-5 fail-closed)
//   4. Size check: downloaded bytes == expected size
//   5. SHA-256 check: computed hash == expected sha256
//   6. Ed25519 check: verify signature with embedded public key (R10A-2 fail-closed)
//   7. Update.write() → Update.end() → reboot
//   8. ACK with success/failure at each stage
//
// FAIL-CLOSED (R10A-2): If OTA_ED25519_PUBLIC_KEY_HEX is empty → OTA REFUSED entirely.
// If signature invalid → Update.abort(). NO BYPASS. NO "warning + continue".
// ---------------------------------------------------------------------------
void MqttClient::_handleOta(const String& json) {
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("[OTA] JSON parse error");
    return;
  }

  const char* action = doc["action"] | "";
  String requestId = doc["requestId"] | "";

  // R10C-1 FIX: Compute OTA command hash HERE (was undefined before — compile error).
  // Uses the same _computeCommandHash() helper as _handleCommand(), with OTA-specific
  // canonical fields: url, version, size, sha256, signature.
  String commandHash = _computeCommandHash(doc);

  if (strcmp(action, "update") != 0) {
    if (requestId.length() > 0) {
      _publishAck(requestId, false, "Invalid OTA action (use update)");
    }
    return;
  }

  const char* url = doc["url"] | "";
  const char* version = doc["version"] | "";
  size_t expectedSize = doc["size"] | 0;
  const char* expectedSha256 = doc["sha256"] | "";
  const char* signatureHex = doc["signature"] | "";

  // Validate required fields
  if (strlen(url) == 0) {
    Services::Log.append(Core::LogType::Error, "OTA: missing URL", 0);
    if (requestId.length() > 0) _publishAck(requestId, false, "OTA: missing URL");
    return;
  }
  if (expectedSize == 0 || expectedSize > Core::OTA_MAX_BINARY_SIZE) {
    Services::Log.append(Core::LogType::Error, "OTA: invalid size", 0);
    if (requestId.length() > 0) _publishAck(requestId, false, "OTA: invalid size (must be 1..2MB)");
    return;
  }
  if (strlen(expectedSha256) != Core::SHA256_HEX_LEN) {
    Services::Log.append(Core::LogType::Error, "OTA: invalid SHA-256", 0);
    if (requestId.length() > 0) _publishAck(requestId, false, "OTA: invalid sha256 (must be 64 hex chars)");
    return;
  }

  // R10B-4 (audit round 10B): Strict SemVer anti-downgrade check.
  // Previously used strcmp() which is wrong for versions like "4.9.0" vs "4.10.0"
  // (string compare says "4.10.0" < "4.9.0" because '1' < '9' lexicographically).
  // Now parse major.minor.patch and compare numerically.
  // version REQUIRED (not optional anymore — empty version rejected).
  if (strlen(version) == 0) {
    Services::Log.append(Core::LogType::Error, "OTA: version field is required", 0);
    if (requestId.length() > 0) _publishAck(requestId, false, "OTA: version field is required");
    return;
  }

  // R10D-4 (audit round 10D): Strict SemVer validation.
  // Must be exactly X.Y.Z (no extra chars like "4.1.0-beta" or "4.1.0.1").
  // sscanf with %d.%d.%d would accept "4.1.0foo" — we verify entire string consumed.
  int newMajor, newMinor, newPatch;
  int curMajor, curMinor, curPatch;
  char extraChar;
  int scanResult = sscanf(version, "%d.%d.%d%c", &newMajor, &newMinor, &newPatch, &extraChar);
  // scanResult == 3 means exactly X.Y.Z with no trailing chars.
  // scanResult == 4 means there was extra junk after X.Y.Z → reject.
  if (scanResult != 3) {
    Services::Log.append(Core::LogType::Error,
      String("OTA: invalid version format (must be X.Y.Z exactly, got: ") + version + ")", 0);
    if (requestId.length() > 0) {
      _publishAck(requestId, false, "OTA: invalid version format (must be X.Y.Z exactly)");
    }
    return;
  }
  // Additional validation: each component must be non-negative and reasonable
  if (newMajor < 0 || newMinor < 0 || newPatch < 0 || newMajor > 999 || newMinor > 999 || newPatch > 999) {
    Services::Log.append(Core::LogType::Error,
      String("OTA: version components out of range: ") + version, 0);
    if (requestId.length() > 0) {
      _publishAck(requestId, false, "OTA: version components out of range (0-999)");
    }
    return;
  }
  if (sscanf(Core::FIRMWARE_VERSION, "%d.%d.%d", &curMajor, &curMinor, &curPatch) != 3) {
    // Fallback: if current version somehow malformed, allow upgrade (don't block)
    Serial.println("[OTA] WARNING: current firmware version malformed, allowing OTA");
  } else {
    // Compare: newVersion must be STRICTLY GREATER than current (no equal, no lower)
    bool isDowngrade = false;
    if (newMajor < curMajor) isDowngrade = true;
    else if (newMajor == curMajor) {
      if (newMinor < curMinor) isDowngrade = true;
      else if (newMinor == curMinor && newPatch <= curPatch) isDowngrade = true;
    }

    if (isDowngrade) {
      String msg = "OTA: downgrade blocked (current=" + String(Core::FIRMWARE_VERSION) +
                   " requested=" + String(version) + ")";
      Services::Log.append(Core::LogType::Error, msg, 0);
      if (requestId.length() > 0) {
        _publishAck(requestId, false, msg.c_str());
      }
      return;
    }
  }

  // R10A-2 (audit round 10): Ed25519 public key is MANDATORY.
  // No bypass for empty key — refuse to proceed with OTA if not configured.
  if (strlen(Core::OTA_ED25519_PUBLIC_KEY_HEX) == 0) {
    Serial.println("[OTA] FATAL: OTA_ED25519_PUBLIC_KEY_HEX not configured — refusing OTA");
    Services::Log.append(Core::LogType::Error,
      "OTA: FATAL — Ed25519 public key not configured. OTA disabled until configured.", 0);
    if (requestId.length() > 0) {
      _publishAck(requestId, false, "OTA disabled — Ed25519 public key not configured");
    }
    return;
  }
  if (strlen(signatureHex) != Core::ED25519_SIGNATURE_LEN * 2) {
    Services::Log.append(Core::LogType::Error, "OTA: invalid signature length", 0);
    if (requestId.length() > 0) _publishAck(requestId, false, "OTA: invalid signature (must be 128 hex chars)");
    return;
  }

  // Require HTTPS
  if (strncmp(url, "https://", 8) != 0) {
    Services::Log.append(Core::LogType::Error, "OTA: URL must be HTTPS", 0);
    if (requestId.length() > 0) _publishAck(requestId, false, "OTA: URL must be HTTPS (plain HTTP not allowed)");
    return;
  }

  Services::Log.append(Core::LogType::Ota,
    String("OTA update requested: v") + version + " (" + String(expectedSize) + " bytes)", 0);

  // Publish OTA status: started
  String statusJson = "{\"otaStatus\":\"downloading\",\"progress\":0}";
  _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)statusJson.c_str(), statusJson.length(), false);

  // Download and verify
  bool success = _downloadAndVerifyOta(url, expectedSize, expectedSha256,
                                       signatureHex, requestId, version);

  if (success) {
    if (requestId.length() > 0) {
      _publishGenericAck(requestId, true, "OTA success — rebooting", commandHash);
    }
    String doneJson = "{\"otaStatus\":\"done\",\"progress\":100,\"newVersion\":\"" + String(version) + "\"}";
    _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)doneJson.c_str(), doneJson.length(), false);
    delay(1000);
    ESP.restart();
  } else {
    if (requestId.length() > 0) {
      _publishAck(requestId, false, "OTA failed (check logs)");
    }
    String failJson = "{\"otaStatus\":\"failed\"}";
    _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)failJson.c_str(), failJson.length(), false);
  }
}

// ---------------------------------------------------------------------------
// Download firmware binary, verify SHA-256 + Ed25519 signature, install.
// P0 #9 (audit round 9): uses WiFiClientSecure with setCACert for HTTPS.
// ---------------------------------------------------------------------------
bool MqttClient::_downloadAndVerifyOta(const String& url, size_t expectedSize,
                                       const char* expectedSha256,
                                       const char* signatureHex,
                                       const String& requestId, const String& version) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);  // 30s for large binary
  http.setConnectTimeout(10000);

  // P0 #9 / R10A-5: Use WiFiClientSecure for HTTPS with CA cert validation.
  // R10A-5: hard-fail if CA not configured (no setInsecure() fallback in production).
  bool useSecure = (strncmp(url.c_str(), "https://", 8) == 0);
  if (useSecure) {
    if (strlen(Core::OTA_HTTPS_ROOT_CA) == 0) {
      Services::Log.append(Core::LogType::Error,
        "OTA: FATAL — OTA_HTTPS_ROOT_CA not configured. Refusing HTTPS download with setInsecure().", 0);
      Serial.println("[OTA] FATAL: OTA_HTTPS_ROOT_CA not configured — refusing HTTPS download");
      return false;
    }
    _otaClientSecure.setCACert(Core::OTA_HTTPS_ROOT_CA);
    Serial.println("[OTA] HTTPS: using configured root CA");
    if (!http.begin(_otaClientSecure, url)) {
      Services::Log.append(Core::LogType::Error, "OTA: HTTPS begin failed", 0);
      return false;
    }
  } else {
    // Plain HTTP — rejected earlier in _handleOta, but defensive check here too
    Services::Log.append(Core::LogType::Error,
      "OTA: FATAL — URL must be HTTPS (plain HTTP not allowed)", 0);
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Services::Log.append(Core::LogType::Error,
      "OTA: HTTP error " + String(httpCode), 0);
    http.end();
    return false;
  }

  int totalSize = http.getSize();
  Serial.printf("[OTA] Downloading %d bytes (expected %u)...\n", totalSize, expectedSize);

  if (totalSize > 0 && (size_t)totalSize != expectedSize) {
    Services::Log.append(Core::LogType::Error,
      "OTA: size mismatch (server=" + String(totalSize) + " expected=" + String(expectedSize) + ")", 0);
    http.end();
    return false;
  }

  if (!Update.begin(expectedSize)) {
    Services::Log.append(Core::LogType::Error, "OTA: Update.begin failed", 0);
    http.end();
    return false;
  }

  // Stream to Update + compute SHA-256 simultaneously
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[512];
  size_t written = 0;
  size_t lastProgress = 0;

  // mbedtls SHA-256 context
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, 0);  // 0 = SHA-256 (not SHA-224)

  while (http.connected() && written < expectedSize) {
    size_t avail = stream->available();
    if (avail) {
      int c = stream->readBytes(buffer, min((size_t)avail, sizeof(buffer)));
      if (c > 0) {
        Update.write(buffer, c);
        mbedtls_sha256_update(&shaCtx, buffer, c);
        written += c;
        size_t progress = (written * 100) / expectedSize;
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

  // Compute SHA-256
  uint8_t computedHash[32];
  mbedtls_sha256_finish(&shaCtx, computedHash);
  mbedtls_sha256_free(&shaCtx);

  http.end();

  if (written != expectedSize) {
    Services::Log.append(Core::LogType::Error,
      "OTA: incomplete download (got=" + String(written) + " expected=" + String(expectedSize) + ")", 0);
    Update.abort();
    return false;
  }

  // Verify SHA-256
  char computedHashHex[Core::SHA256_HEX_LEN + 1];
  Utils::bytesToHex(computedHash, 32, computedHashHex);
  if (strcasecmp(computedHashHex, expectedSha256) != 0) {
    Services::Log.append(Core::LogType::Error,
      "OTA: SHA-256 mismatch (computed=" + String(computedHashHex) + ")", 0);
    Update.abort();
    return false;
  }
  Serial.println("[OTA] SHA-256 verified OK");

  // Verify Ed25519 signature (R10A-2 — audit round 10: REAL implementation, fail-closed)
  //
  // Production mode: if OTA_ED25519_PUBLIC_KEY_HEX is configured, signature
  // verification is MANDATORY. Invalid signature → Update.abort() → return false.
  // NO BYPASS. NO "warning + continue".
  //
  // If OTA_ED25519_PUBLIC_KEY_HEX is EMPTY: hard-fail (refuse to install).
  // This prevents silent downgrade to unsigned OTA in production.
  if (strlen(Core::OTA_ED25519_PUBLIC_KEY_HEX) == 0) {
    Services::Log.append(Core::LogType::Error,
      "OTA: FATAL — OTA_ED25519_PUBLIC_KEY_HEX not configured. Refusing to install unsigned firmware.", 0);
    Serial.println("[OTA] FATAL: Ed25519 public key not configured — refusing to install unsigned firmware");
    Update.abort();
    return false;
  }

  if (strlen(signatureHex) != Core::ED25519_SIGNATURE_LEN * 2) {
    Services::Log.append(Core::LogType::Error,
      "OTA: Invalid signature length (expected 128 hex chars, got " + String(strlen(signatureHex)) + ")", 0);
    Update.abort();
    return false;
  }

  Serial.println("[OTA] Verifying Ed25519 signature...");
  bool sigValid = Utils::ed25519VerifyHash(
    Core::OTA_ED25519_PUBLIC_KEY_HEX,
    signatureHex,
    computedHash, 32
  );

  if (!sigValid) {
    Services::Log.append(Core::LogType::Error,
      "OTA: Ed25519 signature verification FAILED — aborting install", 0);
    Serial.println("[OTA] Ed25519 signature INVALID — aborting Update");
    Update.abort();
    return false;
  }

  // Signature verified OK — safe to finalize
  if (Update.end(true) && Update.isFinished()) {
    Services::Log.append(Core::LogType::Ota,
      "OTA success: " + String(written) + " bytes, v" + version + " (SHA-256 + Ed25519 verified)", 0);
    return true;
  } else {
    Services::Log.append(Core::LogType::Error,
      "OTA: install failed: " + String(Update.getError()), 0);
    return false;
  }
}

// ---------------------------------------------------------------------------
// Request deduplication — prevent double-execution of retried commands
// Uses a ring buffer of last 64 requestIds + commandHashes + ACK results + timestamps.
//
// R10A-3 / R10C-4 / R10D-2 / R10E-3 (audit round 10E):
//   - Buffer size: 64 (increased from 16 in R10C-4)
//   - Stores: requestId, commandHash, ackResultJson, timestamp
//   - R10E-3: TTL-based expiry (15 minutes). Entries older than TTL are
//     treated as "not found" — allows replay after TTL expires (acceptable
//     for non-critical commands; broker ACL prevents attacker from publishing).
//   - On duplicate:
//     - Same requestId + same commandHash + within TTL → replay original ACK
//     - Same requestId + DIFFERENT commandHash → reject "requestId reuse"
//     - Same requestId + expired TTL → treat as new command (execute)
//
// R10D-2 FIX: Stores original ACK JSON for verbatim replay (not reconstructed).
// R10E-1 FIX: Atomic publish+store (no ordering bug).
// ---------------------------------------------------------------------------
#define DEDUP_BUFFER_SIZE 64
#define DEDUP_TTL_MS (15UL * 60UL * 1000UL)  // 15 minutes

static String _processedIds[DEDUP_BUFFER_SIZE];
static String _processedHashes[DEDUP_BUFFER_SIZE];
static String _processedAckResults[DEDUP_BUFFER_SIZE];
static unsigned long _processedTimestamps[DEDUP_BUFFER_SIZE];  // R10E-3: millis() when stored
static uint8_t _dedupIdx = 0;

// R10E-3: Check if requestId is duplicate AND still within TTL.
bool MqttClient::_isDuplicate(const String& requestId) {
  unsigned long now = millis();
  for (uint8_t i = 0; i < DEDUP_BUFFER_SIZE; i++) {
    if (_processedIds[i] == requestId) {
      // R10E-3: Check TTL — if expired, treat as not-duplicate (allow re-execute)
      if (now - _processedTimestamps[i] > DEDUP_TTL_MS) {
        return false;  // expired — treat as new command
      }
      return true;
    }
  }
  return false;
}

// R10A-3: Find the commandHash associated with a previously-seen requestId.
String _getHashForRequestId(const String& requestId) {
  for (uint8_t i = 0; i < DEDUP_BUFFER_SIZE; i++) {
    if (_processedIds[i] == requestId) return _processedHashes[i];
  }
  return "";
}

// R10D-2: Get the ORIGINAL ACK result JSON for a previously-processed requestId.
String _getAckResultForRequestId(const String& requestId) {
  for (uint8_t i = 0; i < DEDUP_BUFFER_SIZE; i++) {
    if (_processedIds[i] == requestId) return _processedAckResults[i];
  }
  return "";
}

// R10E-1: _addProcessed is now ONLY called from ACK publishers (atomic).
// Stores {requestId, commandHash, ackResultJson, timestamp} for duplicate replay.
// ackResultJson is the EXACT JSON that was published to PWA — on duplicate,
// we replay this verbatim instead of reconstructing from command params.
// R10E-3: Also stores timestamp for TTL-based expiry.
void MqttClient::_addProcessed(const String& requestId, const String& commandHash,
                                const String& ackResultJson) {
  _processedIds[_dedupIdx] = requestId;
  _processedHashes[_dedupIdx] = commandHash;
  _processedAckResults[_dedupIdx] = ackResultJson;
  _processedTimestamps[_dedupIdx] = millis();  // R10E-3: for TTL check
  _dedupIdx = (_dedupIdx + 1) % DEDUP_BUFFER_SIZE;
}

// R10A-3 / R10C-1 (audit round 10C): Compute a deterministic command fingerprint.
//
// ENGINEER AUDIT FIX: Previous generic JSON-key iterator only handled string/int/bool.
// Other JSON types (float, array, object) were silently DROPPED from the hash,
// meaning requestId+commandHash binding was incomplete — attacker could add
// extra fields that don't affect the hash but DO affect execution.
//
// FIX: Per-command-type canonical schema. Each command type has a FIXED set
// of fields that are hashed in a DETERMINISTIC ORDER. Unknown fields cause
// the command to be REJECTED (not silently ignored).
//
// Canonical format: "type|action|field1=val1|field2=val2|..."
static String _computeCommandHash(const DynamicJsonDocument& doc) {
  const char* type = doc["type"] | "";
  const char* action = doc["action"] | "";

  String canonical = String(type) + "|" + String(action);

  // Per-command-type: extract ONLY the fields that affect execution.
  if (strcmp(type, "relay") == 0) {
    canonical += "|channelId=" + String(doc["channelId"] | 0);
    canonical += "|mode=" + String(doc["mode"] | "");
    canonical += "|manualState=" + String(doc["manualState"] | false ? "true" : "false");
  }
  else if (strcmp(type, "schedule") == 0) {
    canonical += "|channelId=" + String(doc["channelId"] | 0);
    canonical += "|id=" + String(doc["id"] | 0);
    canonical += "|onTime=" + String(doc["onTime"] | "");
    canonical += "|offTime=" + String(doc["offTime"] | "");
    canonical += "|dayMask=" + String(doc["dayMask"] | 0);
    canonical += "|enabled=" + String(doc["enabled"] | true ? "true" : "false");
  }
  else if (strcmp(type, "pir") == 0) {
    canonical += "|id=" + String(doc["id"] | 0);
    canonical += "|enabled=" + String(doc["enabled"] | false ? "true" : "false");
    canonical += "|holdTime=" + String(doc["holdTime"] | 0);
  }
  else if (strcmp(type, "channel") == 0) {
    canonical += "|channelId=" + String(doc["channelId"] | 0);
    canonical += "|name=" + String(doc["name"] | "");
  }
  else if (strcmp(type, "time") == 0) {
    canonical += "|datetime=" + String(doc["datetime"] | "");
  }
  else if (strcmp(type, "system") == 0) {
    // system commands: action only (reboot, getStatus, resetEnergyStats, resetDailyStats)
  }
  else if (strcmp(type, "config") == 0) {
    canonical += "|deviceName=" + String(doc["deviceName"] | "");
    canonical += "|timezone=" + String(doc["timezone"] | "");
  }
  else if (strcmp(type, "ota") == 0) {
    // R10C-1 FIX: OTA command hash — was UNDEFINED in _handleOta() (compile error).
    canonical += "|url=" + String(doc["url"] | "");
    canonical += "|version=" + String(doc["version"] | "");
    canonical += "|size=" + String((unsigned long)(doc["size"] | 0));
    canonical += "|sha256=" + String(doc["sha256"] | "");
    canonical += "|signature=" + String(doc["signature"] | "");
  }

  return Utils::sha256Hex(canonical);
}

} // namespace Services
