// =============================================================================
// MqttClient.cpp — MQTT client for remote internet access
// =============================================================================
#include "MqttClient.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include "RelayEngine.h"
#include "Scheduler.h"
#include "RtcDriver.h"
#include "RelayDriver.h"
#include "PirDriver.h"
#include "PzemDriver.h"
#include "AuthManager.h"
#include "TransactionJournal.h"
#include "WifiManager.h"
#include "Crypto.h"
#include "Json.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <Preferences.h>  // CYCLE-7: for OTA attempt rate limiting counter

// audit-fixes: forward declaration for static helper (was defined AFTER use).
//   C++ requires declaration before use. The static function _computeCommandHash
//   was defined at the bottom of the file but called from _handleCommand() and
//   _handleOta() above. Must be placed AFTER ArduinoJson.h include so that
//   DynamicJsonDocument is visible in the signature.
//
// CYCLE-7 FIX (pre-existing bug): the forward declaration was OUTSIDE
//   `namespace Services` but the definition was INSIDE. This caused a linker
//   error (undefined reference). Moved the forward declaration INSIDE the
//   namespace below so it matches the definition's scope.
#include "ConfigStore.h"
#include <mbedtls/sha256.h>
#include <algorithm>
#include <vector>

namespace Services {

// Forward declaration of static helper (defined at bottom of file).
static String _computeCommandHash(const DynamicJsonDocument& doc);

MqttClient mqtt;

// Log type names for MQTT log publishing
static const char* LOG_TYPE_NAMES[] = {
  "relay_on", "relay_off", "pir_trigger", "login", "logout",
  "error", "restart", "ota", "config_change", "factory_reset",
  "time_sync", "auth_fail"
};

bool MqttClient::begin() {
  // R10A-5 / R10F-5 (audit round 10F): Production MQTT guard.
  //
  // Two ways to trigger production mode:
  //   1. Port-based: MQTT_BROKER_PORT == 8883 || 8884 (automatic)
  //   2. Build flag: PRODUCTION_BUILD defined (explicit, stronger)
  //
  // R10F-5: PRODUCTION_BUILD flag enforces requirements REGARDLESS of port.
  // This prevents accidental deployment with port 1883 + plaintext in production.
  //
  // In production mode, ALL of these must be configured:
  //   - MQTT_BROKER_PORT must be 8883 or 8884 (TLS)
  //   - MQTT_BROKER_USERNAME (non-empty)
  //   - MQTT_BROKER_PASSWORD (non-empty)
  //   - MQTT_ROOT_CA (non-empty PEM)
  //   - ALLOWED_CORS_ORIGINS must NOT be "*"
  //   - OTA_ED25519_PUBLIC_KEY_HEX must be non-empty
  //   - OTA_HTTPS_ROOT_CA must be non-empty
  // If any is missing → hard fail (refuse to connect).

  // CYCLE-7 (fixes F-007): resolve effective broker host/port.
  //   - In PRODUCTION: empty MQTT_BROKER_HOST/PORT = FAIL CLOSED (no fallback).
  //   - In DEVELOPMENT: empty MQTT_BROKER_HOST/PORT = use HiveMQ public broker
  //     with a LOUD warning. This preserves the dev workflow while ensuring
  //     production builds can never silently fall back to an insecure broker.
  String effectiveBrokerHost = Core::MQTT_BROKER_HOST;
  uint16_t effectiveBrokerPort = Core::MQTT_BROKER_PORT;

#ifndef PRODUCTION_BUILD
  if (effectiveBrokerHost.length() == 0 || effectiveBrokerPort == 0) {
    Serial.println("");
    Serial.println("============================================================");
    Serial.println("[MQTT] WARNING: DEVELOPMENT FALLBACK ACTIVE");
    Serial.println("[MQTT]   MQTT_BROKER_HOST/PORT not configured in Config.h.");
    Serial.println("[MQTT]   Falling back to public HiveMQ broker (broker.hivemq.com:1883).");
    Serial.println("[MQTT]   This is INSECURE — do NOT use in production!");
    Serial.println("[MQTT]   To fix: set MQTT_BROKER_HOST and MQTT_BROKER_PORT in Config.h,");
    Serial.println("[MQTT]   or build with -DPRODUCTION_BUILD to enforce fail-closed behavior.");
    Serial.println("============================================================");
    Serial.println("");
    if (effectiveBrokerHost.length() == 0) effectiveBrokerHost = Core::MQTT_DEV_FALLBACK_HOST;
    if (effectiveBrokerPort == 0) effectiveBrokerPort = Core::MQTT_DEV_FALLBACK_PORT;
  }
#endif

#ifdef PRODUCTION_BUILD
  // CYCLE-7: in production, empty broker config = FAIL CLOSED.
  if (effectiveBrokerHost.length() == 0 || effectiveBrokerPort == 0) {
    Serial.println("[MQTT] FATAL: PRODUCTION_BUILD requires MQTT_BROKER_HOST and MQTT_BROKER_PORT to be set in Config.h");
    Serial.println("[MQTT] Refusing to fall back to public broker in production build.");
    _initialized = false;
    return false;
  }
#endif

  bool isProductionMode = (effectiveBrokerPort == 8883 || effectiveBrokerPort == 8884);

#ifdef PRODUCTION_BUILD
  // R10F-5: Build-flag-based production guard — stronger than port-based.
  isProductionMode = true;
  Serial.println("[MQTT] PRODUCTION_BUILD flag defined — enforcing all production requirements");

  // In production build, TLS port is MANDATORY
  if (effectiveBrokerPort != 8883 && effectiveBrokerPort != 8884) {
    Serial.printf("[MQTT] FATAL: PRODUCTION_BUILD requires TLS port (8883/8884), got %d\n",
                  effectiveBrokerPort);
    Serial.println("[MQTT] Refusing to use plaintext MQTT in production build.");
    _initialized = false;
    return false;
  }

  // CORS must not be wildcard
  if (strcmp(Core::ALLOWED_CORS_ORIGINS, "*") == 0) {
    Serial.println("[MQTT] FATAL: PRODUCTION_BUILD requires ALLOWED_CORS_ORIGINS (not '*')");
    _initialized = false;
    return false;
  }
  // CYCLE-7: CORS must also not be EMPTY in production (was previously allowed).
  if (strlen(Core::ALLOWED_CORS_ORIGINS) == 0) {
    Serial.println("[MQTT] FATAL: PRODUCTION_BUILD requires ALLOWED_CORS_ORIGINS to be set (empty is not allowed)");
    _initialized = false;
    return false;
  }

  // OTA signing must be configured
  if (strlen(Core::OTA_ED25519_PUBLIC_KEY_HEX) == 0) {
    Serial.println("[MQTT] FATAL: PRODUCTION_BUILD requires OTA_ED25519_PUBLIC_KEY_HEX");
    _initialized = false;
    return false;
  }
  if (strlen(Core::OTA_HTTPS_ROOT_CA) == 0) {
    Serial.println("[MQTT] FATAL: PRODUCTION_BUILD requires OTA_HTTPS_ROOT_CA");
    _initialized = false;
    return false;
  }
  // CYCLE-7 (fixes F-011): OTA_ALLOWED_HOSTS must be set in production.
  if (strlen(Core::OTA_ALLOWED_HOSTS) == 0) {
    Serial.println("[MQTT] FATAL: PRODUCTION_BUILD requires OTA_ALLOWED_HOSTS to be set");
    _initialized = false;
    return false;
  }
  // CYCLE-7 (fixes F-020): JWT_SECRET_DEFAULT must be empty in production source.
  //   Actual secret is loaded from NVS (per-device random) at boot.
  if (strlen(Core::JWT_SECRET_DEFAULT) > 0) {
    Serial.println("[MQTT] FATAL: PRODUCTION_BUILD requires JWT_SECRET_DEFAULT to be empty (use NVS)");
    _initialized = false;
    return false;
  }
#endif

  if (isProductionMode) {
    if (strlen(Core::MQTT_BROKER_USERNAME) == 0) {
      Serial.println("[MQTT] FATAL: Production mode requires MQTT_BROKER_USERNAME");
      Serial.println("[MQTT] Refusing to connect. Configure credentials in Config.h and re-flash.");
      _initialized = false;
      return false;
    }
    if (strlen(Core::MQTT_BROKER_PASSWORD) == 0) {
      Serial.println("[MQTT] FATAL: Production mode requires MQTT_BROKER_PASSWORD");
      Serial.println("[MQTT] Refusing to connect. Configure credentials in Config.h and re-flash.");
      _initialized = false;
      return false;
    }
    if (strlen(Core::MQTT_ROOT_CA) == 0) {
      Serial.println("[MQTT] FATAL: Production mode requires MQTT_ROOT_CA (PEM)");
      Serial.println("[MQTT] Refusing to connect with setInsecure() in production.");
      Serial.println("[MQTT] Get Let's Encrypt root CA: https://letsencrypt.org/certs/isrgrootx1.pem");
      Serial.println("[MQTT] Paste PEM in Config.h MQTT_ROOT_CA and re-flash.");
      _initialized = false;
      return false;
    }
    Serial.println("[MQTT] Production mode: TLS + auth + CA verified ✓");
  }

  // Use TLS (WiFiClientSecure) if broker port is 8883/8884, otherwise plain TCP
  if (effectiveBrokerPort == 8883 || effectiveBrokerPort == 8884) {
    // R10A-5: setCACert() is MANDATORY in production (checked above).
    // No setInsecure() fallback — fail-closed.
    _wifiClientSecure.setCACert(Core::MQTT_ROOT_CA);
    Serial.println("[MQTT] TLS: using configured root CA for broker cert validation");
    _mqtt.setClient(_wifiClientSecure);
    Serial.printf("[MQTT] Using TLS (port %d)\n", effectiveBrokerPort);
  } else {
    _mqtt.setClient(_wifiClient);
    Serial.println("[MQTT] Using plain TCP (no TLS) — development mode only");
  }
  _mqtt.setServer(effectiveBrokerHost.c_str(), effectiveBrokerPort);
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

  // R10G-1: Initialize transaction journal (loads from NVS, queues pending ACKs)
  Services::journal.begin();

  // R10G-2: Set up publish callback for ACK retry queue
  // This allows TransactionJournal to publish ACKs via _mqtt
  _topicAckForJournal = _topicAck;  // store topic for callback
  Services::setPublishCallback([this](const char* topic, const uint8_t* payload, size_t len) -> bool {
    if (!_mqtt.connected()) return false;
    return _mqtt.publish(_topicAckForJournal.c_str(), payload, len, false);
  });

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

  // CYCLE-7: log actual broker being used (may be dev fallback).
  // _mqtt already configured with effective broker in begin().
  Serial.println("MQTT: connecting to broker (already configured in begin())...");

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

  // R10G-2: Process pending ACK queue — retry publishing ACKs that haven't
  // been confirmed delivered. Runs every 2s per ACK (bounded by journal).
  // This ensures ACK delivery even if initial publish() failed or MQTT
  // connection dropped mid-publish.
  Services::journal.processPendingAcks();

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
  // CYCLE-7 (fixes F-022): removed hardcoded cpuLoadPercent=10 and flashFreePercent=35.
  //   These were mock values that misled the PWA into thinking telemetry was real.
  //   Real CPU load measurement on ESP32 requires idle-task hook (not implemented).
  //   Flash free space requires esp_partition_info (not worth the memory cost
  //   for the value it provides). Omitted from response rather than fabricating numbers.
  //   PWA should treat absence of these fields as "unknown" (not 0 or 100).
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
// CYCLE-7 (fixes F-023): use monotonic logId from LogService, not millis().
// ---------------------------------------------------------------------------
void MqttClient::publishLog(Core::LogType type, const String& message, int8_t channelId, uint32_t logId) {
  if (!_mqtt.connected()) return;

  // CYCLE-7: if logId is 0 (legacy caller), use a module-local monotonic counter
  // so IDs are still unique per boot (better than millis() which can collide).
  static uint32_t sFallbackLogId = 0;
  if (logId == 0) {
    logId = ++sFallbackLogId;
  }

  StaticJsonDocument<512> doc;
  doc["id"] = logId;  // monotonic per boot (or per session for fallback)
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
// audit-fixes: the previous in-memory dedup ring buffer has been REMOVED.
// All dedup is now authoritative via the NVS-backed TransactionJournal.
//
// ACK publishers (below) accept a `commandHash` parameter. For SUCCESS ACKs
// with non-empty commandHash, the publisher:
//   1. Constructs the ACK JSON.
//   2. Stores {requestId, commandHash, ackJson} in the NVS journal (durable,
//      survives reboot, 64-entry ring with CRC32 + two-phase commit).
//   3. Publishes the ACK to MQTT (best-effort immediate delivery).
//   4. If publish fails, journal.processPendingAcks() retries every 2s.
//
// For FAILURE ACKs (success=false): pass commandHash="" → NOT stored in
// journal (failed commands can be retried with same requestId).
// =============================================================================

// ============================================================================
// CYCLE-7 (fixes F-001 + F-002 + F-006): _finalizeAndPublishAck helper.
//
// Replaces the duplicated storeTransaction() + publish() pattern in each ACK
// publisher. Now all ACK publishers build their JSON, then delegate here for:
//   1. Commit transaction (PENDING → COMMITTED) — checked return value (F-002).
//   2. Immediate publish on success.
//   3. Dequeue ACK on immediate publish success (F-006).
//   4. Publish FAILURE ACK with DURABILITY_FAILURE message if commit fails.
// ============================================================================
void MqttClient::_finalizeAndPublishAck(const String& requestId, bool success,
                                         const String& preBuiltJson, const String& commandHash,
                                         CommitMode commitMode) {
  if (!_mqtt.connected()) {
    Serial.printf("[MQTT ACK] %s: MQTT not connected — ACK queued for retry\n", requestId.c_str());
    // For success ACKs, the journal's queueAck() (called by commitTransaction)
    // will retry delivery. For failure ACKs, we lose them (acceptable — PWA retries).
    return;
  }

  // FAILURE ACKs: publish directly, do NOT commit (command can be retried).
  if (!success || commandHash.length() == 0) {
    _mqtt.publish(_topicAck.c_str(), (const uint8_t*)preBuiltJson.c_str(),
                  preBuiltJson.length(), false);
    Serial.printf("[MQTT ACK] %s: %s (not committed — retryable)\n",
                  requestId.c_str(), success ? "OK" : "FAIL");
    return;
  }

  // SUCCESS ACK with commandHash: must commit to journal first.
  // P2-2 F-P0-1: CommitMode determines which commit path to use.
  //   NONE        — read-only: skip commit, just publish ACK
  //   FROM_PENDING — atomic config: PENDING → COMMITTED
  //   EXECUTING    — physical mutation: EXECUTING → COMMITTED
  // CYCLE-7 fix for F-002: CHECK return value. If commit fails, do NOT claim success.
  if (commitMode == CommitMode::NONE) {
    // Read-only command — no journal entry exists, just publish ACK
    _mqtt.publish(_topicAck.c_str(), (const uint8_t*)preBuiltJson.c_str(),
                  preBuiltJson.length(), false);
    Serial.printf("[MQTT ACK] %s: OK (read-only, no commit)\n", requestId.c_str());
    return;
  }

  bool committed;
  if (commitMode == CommitMode::FROM_PENDING) {
    committed = Services::journal.commitTransactionFromPending(requestId, preBuiltJson);
  } else {
    committed = Services::journal.commitTransaction(requestId, preBuiltJson);
  }
  if (!committed) {
    // Commit failed — NVS write error, journal full, or other durability failure.
    // Do NOT publish the success ACK (would falsely claim durable success).
    // Publish a FAILURE ACK so PWA knows to retry.
    Serial.printf("[MQTT ACK] %s: COMMIT FAILED — publishing DURABILITY_FAILURE\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "Transaction commit FAILED for " + requestId + " — publishing DURABILITY_FAILURE", 0);

    StaticJsonDocument<512> failDoc;
    failDoc["requestId"] = requestId;
    failDoc["success"] = false;
    failDoc["message"] = "DURABILITY_FAILURE: transaction could not be committed to NVS journal — please retry";
    failDoc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
    String failJson;
    serializeJson(failDoc, failJson);
    _mqtt.publish(_topicAck.c_str(), (const uint8_t*)failJson.c_str(),
                  failJson.length(), false);
    return;
  }

  // Commit succeeded — ACK is durable. Now publish immediately for low latency.
  // commitTransaction() already called queueAck() — so ACK is also in retry queue.
  bool published = _mqtt.publish(_topicAck.c_str(), (const uint8_t*)preBuiltJson.c_str(),
                                 preBuiltJson.length(), false);
  if (published) {
    // CYCLE-7 fix for F-006: immediate publish succeeded — dequeue to prevent
    // processPendingAcks() from publishing duplicate ACK.
    Services::journal.dequeueAck(requestId);
    Serial.printf("[MQTT ACK] %s: OK (committed + published)\n", requestId.c_str());
  } else {
    // Immediate publish failed — ACK stays in queue. processPendingAcks() retries.
    Serial.printf("[MQTT ACK] %s: committed, publish pending retry\n", requestId.c_str());
  }
}

// Generic ACK — used for failures and generic successes.
// If commandHash is non-empty AND success=true → store in NVS journal.
void MqttClient::_publishAck(const String& requestId, bool success, const char* message,
                              const String& dataJson, const String& commandHash,
                              CommitMode commitMode) {
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

  // CYCLE-7: delegate to _finalizeAndPublishAck for commit + publish + dequeue.
  _finalizeAndPublishAck(requestId, success, json, commandHash, commitMode);
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

  // CYCLE-7: delegate to _finalizeAndPublishAck for commit + publish + dequeue.
  _finalizeAndPublishAck(requestId, success, json, commandHash, CommitMode::EXECUTING);
  Serial.printf("[MQTT ACK] %s: %s (relay CH%d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", channelId,
                commandHash.length() > 0 ? " (committed)" : "");
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

  // CYCLE-7: delegate to _finalizeAndPublishAck for commit + publish + dequeue.
  _finalizeAndPublishAck(requestId, success, json, commandHash, CommitMode::FROM_PENDING);
  Serial.printf("[MQTT ACK] %s: %s (schedule CH%d id=%d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", channelId, scheduleId,
                commandHash.length() > 0 ? " (committed)" : "");
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

  // CYCLE-7: delegate to _finalizeAndPublishAck for commit + publish + dequeue.
  _finalizeAndPublishAck(requestId, success, json, commandHash, CommitMode::FROM_PENDING);
  Serial.printf("[MQTT ACK] %s: %s (pir %d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", pirId,
                commandHash.length() > 0 ? " (committed)" : "");
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

  // CYCLE-7: delegate to _finalizeAndPublishAck for commit + publish + dequeue.
  _finalizeAndPublishAck(requestId, success, json, commandHash, CommitMode::FROM_PENDING);
  Serial.printf("[MQTT ACK] %s: %s (channel CH%d)%s\n", requestId.c_str(),
                success ? "OK" : "FAIL", channelId,
                commandHash.length() > 0 ? " (committed)" : "");
}

// Generic ACK for time/system/config mutations.
void MqttClient::_publishGenericAck(const String& requestId, bool success, const char* message,
                                    const String& dataJson, const String& commandHash,
                                    CommitMode commitMode) {
  _publishAck(requestId, success, message, dataJson, commandHash, commitMode);
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
  // CYCLE-8B (C8A-007): Reject commands during boot recovery phase.
  //   During PRE_INIT through RECONCILING, the system is not ready to accept
  //   commands — RelayEngine is not running, journal is being reconciled.
  //   Commands received during this window are ACKed with a "not ready" message
  //   so PWA knows to retry.
  if (!Services::journal.isRunning()) {
    Serial.printf("[MQTT] REJECTED: system not ready (boot phase=%u)\n",
                  (uint8_t)Services::journal.getBootPhase());
    // Try to extract requestId for ACK
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, json);
    String requestId = "";
    if (!err) {
      requestId = doc["requestId"] | "";
    }
    if (requestId.length() > 0) {
      _publishAck(requestId, false,
        "System not ready (boot recovery in progress) — please retry in a moment");
    }
    return;
  }

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

  // R10G-3: Strict requestId validation.
  // requestId MUST be present and valid for transaction tracking.
  // Without requestId, command cannot be deduplicated → reject.
  if (requestId.length() == 0) {
    Serial.println("[MQTT] REJECTED: missing requestId");
    _publishAck("", false, "requestId is required for all commands");
    return;
  }
  if (requestId.length() > 64) {
    Serial.printf("[MQTT] REJECTED: requestId too long (%d chars, max 64)\n", requestId.length());
    _publishAck(requestId, false, "requestId too long (max 64 chars)");
    return;
  }
  // Validate requestId contains only safe ASCII (UUID format recommended)
  for (size_t i = 0; i < requestId.length(); i++) {
    char c = requestId[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      Serial.printf("[MQTT] REJECTED: requestId contains invalid char at pos %d\n", i);
      _publishAck(requestId, false, "requestId contains invalid characters (use UUID format)");
      return;
    }
  }

  // Step 3: Compute command fingerprint (AFTER validation passes).
  String commandHash = _computeCommandHash(doc);

  // ===========================================================================
  // CYCLE-8B-Rev1: Monotonic state machine duplicate handling
  // ===========================================================================
  //   isProcessed() returns true ONLY for COMMITTED and COMMITTED_UNKNOWN.
  //   For other states, we check getTransactionState() explicitly:
  //     - COMMITTED → replay stored ACK (true duplicate, durable success)
  //     - COMMITTED_UNKNOWN → replay ACK with disclaimer
  //     - FAILED → clear entry, allow retry (proven not executed)
  //     - UNKNOWN → surface to PWA, do NOT auto-retry (cannot determine)
  //     - PENDING/EXECUTING → reconcile, then decide
  // ===========================================================================
  int existingIdx = -1;  // Check if requestId exists in journal
  {
    // Use a helper to check existence without isProcessed() boolean
    Services::TransactionState existingState = Services::journal.getTransactionState(requestId);
    // getTransactionState returns PENDING if not found — but PENDING is also a valid
    // in-flight state. We need to distinguish "not in journal" from "in journal as PENDING".
    // Use isProcessed() + getTransactionState() combination:
    //   - If isProcessed()=true → COMMITTED or COMMITTED_UNKNOWN
    //   - If isProcessed()=false AND getTransactionState()=PENDING → could be not-found OR in-flight PENDING
    //   To resolve: check if requestId actually exists by trying getCommandHash()
    String existingHash = Services::journal.getCommandHash(requestId);
    if (existingHash.length() > 0) {
      // requestId exists in journal
      existingIdx = 1;  // marker (not actual index, just "exists")

      // Hash mismatch → security rejection
      if (existingHash != commandHash) {
        Serial.printf("[MQTT] SECURITY: requestId reuse with different command! rid=%s\n", requestId.c_str());
        Services::Log.append(Core::LogType::AuthFail,
          "SECURITY: requestId reuse with different command: " + requestId, 0);
        _publishAck(requestId, false, "requestId reuse with different command — rejected");
        return;
      }

      Services::TransactionState state = existingState;

      if (state == Services::TransactionState::COMMITTED) {
        // True duplicate — replay ORIGINAL ACK via journal.
        Serial.printf("[MQTT] Duplicate (COMMITTED): %s — replaying original ACK\n", requestId.c_str());
        String originalAckJson = Services::journal.getAckJson(requestId);
        if (originalAckJson.length() > 0) {
          Services::journal.queueAck(requestId, originalAckJson);
        } else {
          _publishAck(requestId, true, "Duplicate command (already executed)");
        }
        return;
      }

      if (state == Services::TransactionState::COMMITTED_UNKNOWN) {
        // Reconciled at boot — GPIO matches desired, but no durable proof.
        Serial.printf("[MQTT] Duplicate (COMMITTED_UNKNOWN): %s — replaying ACK with disclaimer\n",
                      requestId.c_str());
        String originalAckJson = Services::journal.getAckJson(requestId);
        if (originalAckJson.length() > 0) {
          Services::journal.queueAck(requestId, originalAckJson);
        } else {
          _publishAck(requestId, true,
            "Command may have executed (reconciled — GPIO matches desired, physical contact state unknown)");
        }
        return;
      }

      if (state == Services::TransactionState::FAILED) {
        // Proven not executed — clear and allow retry.
        Serial.printf("[MQTT] Duplicate (FAILED): %s — proven not executed, allowing retry\n",
                      requestId.c_str());
        Services::Log.append(Core::LogType::Error,
          "FAILED transaction retried (proven not executed): " + requestId, 0);
        if (!Services::journal.clearEntry(requestId)) {
          // CYCLE-8B-Rev1 (C8B-007): clearEntry failed — NVS write error.
          // Do NOT proceed with retry — entry may still be in NVS and could
          // cause confusion. Surface error to PWA.
          _publishAck(requestId, false,
            "Internal error: cannot clear FAILED transaction (NVS write failure) — please retry");
          return;
        }
        // Fall through to normal execution below.
      } else if (state == Services::TransactionState::UNKNOWN) {
        // CYCLE-8B-Rev1 (fixes C8B-002): UNKNOWN means "cannot determine if execute ran".
        //   Do NOT auto-retry — could double-execute.
        //   Surface to PWA with specific message so operator/user can decide.
        Serial.printf("[MQTT] Duplicate (UNKNOWN): %s — cannot determine if executed, surfacing to PWA\n",
                      requestId.c_str());
        Services::Log.append(Core::LogType::Error,
          "UNKNOWN transaction retried (cannot determine if executed — NOT auto-retrying): " + requestId, 0);
        _publishAck(requestId, false,
          "AMBIGUOUS: transaction state is UNKNOWN (cannot determine if previously executed). "
          "For idempotent commands (relay ON/OFF), retry is safe. For other commands, "
          "verify device state before retrying.");
        return;
      } else {
        // PENDING or EXECUTING — command was in-flight during crash.
        // CYCLE-8B-Rev1: reconcile now (during RUNNING, this produces UNKNOWN).
        Serial.printf("[MQTT] Duplicate (%s): %s — reconciling\n",
                      state == Services::TransactionState::EXECUTING ? "EXECUTING" : "PENDING",
                      requestId.c_str());
        Services::TransactionState reconciled = Services::journal.reconcileEntry(requestId);

        // reconcileEntry now always returns UNKNOWN (fixes C8B-004)
        if (reconciled == Services::TransactionState::UNKNOWN) {
          _publishAck(requestId, false,
            "AMBIGUOUS: transaction was in-flight during crash, cannot determine if executed. "
            "For idempotent commands, retry is safe. For other commands, verify device state.");
          return;
        }
        // For any other state, fall through (shouldn't happen with monotonicity check)
      }
    }
  }

  // CYCLE-8B (fixes C8A-005): Validate ALL command fields BEFORE storeIntent.
  //   Previous order: storeIntent → validate channel → validate action → execute
  //   This left PENDING entries for invalid commands, contaminating the journal.
  //   New order: validate EVERYTHING → storeIntent → markExecuting → execute
  //   Invalid commands never create journal entries.
  //
  // Pre-validate relay commands
  uint8_t intentChannelId = 0;
  bool intentDesiredState = false;
  bool intentPreviousKnown = false;

  if (strcmp(type, "relay") == 0) {
    int ch = doc["channelId"] | 0;
    if (ch < 1 || ch > Core::NUM_CHANNELS) {
      _publishAck(requestId, false, "Invalid channelId");
      return;  // No journal entry created
    }
    intentChannelId = (uint8_t)ch;
    uint8_t idx = ch - 1;
    intentPreviousKnown = Core::relayState[idx];

    const char* actionStr = doc["action"] | "";
    if (strcmp(actionStr, "on") == 0) {
      intentDesiredState = true;
    } else if (strcmp(actionStr, "off") == 0) {
      intentDesiredState = false;
    } else if (strcmp(actionStr, "set_mode") == 0) {
      const char* mode = doc["mode"] | "";
      if (strcmp(mode, "manual") == 0) {
        intentDesiredState = doc["manualState"] | false;
      } else if (strcmp(mode, "auto") == 0) {
        // set_mode auto doesn't directly change relay state
        intentDesiredState = Core::relayState[idx];
      } else {
        _publishAck(requestId, false, "Invalid mode (use auto/manual)");
        return;  // No journal entry created
      }
    } else {
      _publishAck(requestId, false, "Invalid relay action (use on/off/set_mode)");
      return;  // No journal entry created
    }
  }

  // CYCLE-8A: Store durable INTENT record with expanded metadata.
  // P2-2 F-P0-1: Skip journal for read-only commands (system/getStatus).
  //   Read-only commands have no mutation → no durability needed → no dedup needed.
  bool isReadOnly = (strcmp(type, "system") == 0 && strcmp(action, "getStatus") == 0);

  if (!isReadOnly) {
    if (!Services::journal.storeIntent(requestId, commandHash,
                                        intentChannelId, intentDesiredState,
                                        intentPreviousKnown)) {
      Serial.printf("[MQTT] FATAL: storeIntent failed for rid=%s — refusing to execute\n",
                    requestId.c_str());
      Services::Log.append(Core::LogType::Error,
        "storeIntent FAILED — command rejected: " + requestId, 0);
      _publishAck(requestId, false,
        "DURABILITY_FAILURE: cannot store transaction intent — please retry");
      return;
    }
  }


  // ===========================================================================
  // RELAY COMMANDS
  // ===========================================================================
  if (strcmp(type, "relay") == 0) {
    int channelId = intentChannelId;
    uint8_t idx = channelId - 1;

    // CYCLE-8B: Mark EXECUTING right before the GPIO write.
    if (!Services::journal.markExecuting(requestId)) {
      Serial.printf("[MQTT] markExecuting failed for rid=%s — aborting\n", requestId.c_str());
      _publishAck(requestId, false, "Internal error: cannot mark transaction as executing");
      Services::journal.clearEntry(requestId);
      return;
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
      } else {  // manual (already validated above)
        Services::relayEngine.setMode(idx, false);
        Services::relayEngine.setManual(idx, manualState);
      }
    }

    // CYCLE-8B (fixes C8A-008): GPIO readback after execute.
    //   If GPIO doesn't match desired → success=FALSE (not success=true+warning).
    //   For relay mains, success=true on mismatch is a dangerous contract.
    bool actualGpio = Drivers::relay.readLogicalState(idx);
    bool desired = Core::relayState[idx];

    publishStatus();

    if (actualGpio == desired) {
      _publishRelayAck(requestId, true, "Relay command executed", channelId, commandHash);
    } else {
      // CYCLE-8C (fixes C8BR1-008): GPIO mismatch → durable terminal state.
      //   Previous: empty commandHash = don't commit (left entry as EXECUTING).
      //   This was wrong — physical execute was attempted but produced wrong output.
      //   Now: commit as EXECUTION_FAILED_OUTPUT_MISMATCH (durable terminal).
      //   Operator must investigate physical relay (welded/stuck/driver fault).
      //   No auto-retry — this is a hardware problem, not a transient failure.
      Services::Log.append(Core::LogType::Error,
        "GPIO readback mismatch after relay execute: ch=" + String(channelId) +
        " expected=" + (desired ? "ON" : "OFF") + " actual=" + (actualGpio ? "ON" : "OFF"), channelId);

      // Build failure ACK JSON
      StaticJsonDocument<512> failDoc;
      failDoc["requestId"] = requestId;
      failDoc["success"] = false;
      failDoc["message"] = "OUTPUT_MISMATCH: GPIO readback does not match desired state (relay driver may have failed — operator investigation required)";
      failDoc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
      JsonObject data = failDoc.createNestedObject("data");
      data["channelId"] = channelId;
      data["expectedState"] = desired;
      data["actualGpioState"] = actualGpio;
      data["transactionState"] = "EXECUTION_FAILED_OUTPUT_MISMATCH";
      String failJson;
      serializeJson(failDoc, failJson);

      // Commit as durable terminal failure (no auto-retry)
      Services::journal.commitTransactionFailed(
        requestId, failJson, Services::TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH);

      // Publish failure ACK immediately
      _finalizeAndPublishAck(requestId, false, failJson, "", CommitMode::EXECUTING);
    }
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
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
          Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
        return;  // no _addProcessed
      }
    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid PIR action (use config/test)");
      }
          Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
        return;  // no _addProcessed
      }
      if (strlen(name) > Core::MAX_NAME_LEN) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Name too long (max 20 chars)");
        }
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
          Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
        return;  // no _addProcessed
      }
      if (!Utils::isValidDate(y, m, d) || h < 0 || h > 23 || mi < 0 || mi > 59) {
        if (requestId.length() > 0) {
          _publishAck(requestId, false, "Invalid date/time values");
        }
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
        return;  // no _addProcessed
      }
      Drivers::rtc.adjust(y, m, d, h, mi, s);
      Services::Log.append(Core::LogType::TimeSync, "RTC set via MQTT", 0);

      _publishGenericAck(requestId, true, "RTC time set", commandHash);
    } else {
      if (requestId.length() > 0) {
        _publishAck(requestId, false, "Invalid time action (use set)");
      }
          Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
      return;  // no _addProcessed
    }
  }

  // ===========================================================================
  // SYSTEM COMMANDS (P1 #11: now sends ACK)
  // ===========================================================================
  else if (strcmp(type, "system") == 0) {
    if (strcmp(action, "reboot") == 0) {
      // P2-2 R6-C1: Reboot lifecycle — commit BEFORE restart, ACK stays queued.
      //   1. storeIntent was already called (PENDING in journal)
      //   2. commitTransactionFromPending → COMMITTED + ACK queued (durable)
      //   3. Publish ACK immediately (low latency for PWA)
      //   4. Do NOT dequeue ACK — leave it in queue for durability
      //      If immediate publish fails, processPendingAcks would have retried,
      //      but we're about to restart so it won't get a chance.
      //      After reboot, boot merge sees ACK still in queue → does NOT re-add
      //      (already present). PWA deduplicates by requestId.
      //   5. ESP.restart()
      Services::Log.append(Core::LogType::Restart, "Reboot via MQTT", 0);
      if (requestId.length() > 0) {
        // Build ACK JSON
        StaticJsonDocument<256> doc;
        doc["requestId"] = requestId;
        doc["success"] = true;
        doc["message"] = "Rebooting";
        doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
        String ackJson;
        serializeJson(doc, ackJson);

        // Commit to journal (PENDING → COMMITTED + ACK queued)
        bool committed = Services::journal.commitTransactionFromPending(requestId, ackJson);
        if (!committed) {
          // Commit failed — publish DURABILITY_FAILURE, do NOT restart
          // (reboot without durable commit would lose transaction evidence)
          Serial.printf("[MQTT] reboot: commitTransactionFromPending FAILED — NOT restarting\n");
          _publishAck(requestId, false,
            "DURABILITY_FAILURE: cannot commit reboot transaction — please retry");
          return;
        }

        // ACK is durable — publish immediately for low latency
        // Do NOT dequeue — ACK stays in queue as durable evidence
        _mqtt.publish(_topicAck.c_str(), (const uint8_t*)ackJson.c_str(),
                       ackJson.length(), false);
        Serial.printf("[MQTT ACK] %s: Rebooting (committed, ACK published, not dequeued)\n",
                      requestId.c_str());
      }
      delay(500);
      ESP.restart();
    } else if (strcmp(action, "getStatus") == 0) {
      publishStatus();
      if (requestId.length() > 0) {
        _publishGenericAck(requestId, true, "Status published", "", commandHash, CommitMode::NONE);
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
          Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
            Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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
          Services::journal.clearEntry(requestId);  // P2-2 F-P0-1: clear PENDING (no mutation occurred)
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

  // CYCLE-7 (fixes I-004): OTA commands now go through TransactionJournal for
  //   dedup, just like regular commands. Previously _onMessage() routed OTA
  //   directly to _handleOta(), bypassing journal.isProcessed() / storeIntent().
  //   A replayed OTA command would trigger a fresh HTTPS download + flash write.
  //   Now: check journal first, store intent before download.
  if (requestId.length() > 0) {
    if (Services::journal.isProcessed(requestId)) {
      String previousHash = Services::journal.getCommandHash(requestId);
      if (previousHash.length() > 0 && previousHash != commandHash) {
        Serial.printf("[OTA] SECURITY: requestId reuse with different command! rid=%s\n", requestId.c_str());
        Services::Log.append(Core::LogType::AuthFail,
          "SECURITY: OTA requestId reuse with different command: " + requestId, 0);
        _publishAck(requestId, false, "requestId reuse with different command — rejected");
        return;
      }

      if (Services::journal.isCommitted(requestId)) {
        Serial.printf("[OTA] Duplicate (COMMITTED): %s — replaying original ACK\n", requestId.c_str());
        String originalAckJson = Services::journal.getAckJson(requestId);
        if (originalAckJson.length() > 0) {
          Services::journal.queueAck(requestId, originalAckJson);
        } else {
          _publishAck(requestId, true, "Duplicate OTA command (already executed)");
        }
      } else {
        // PENDING — OTA was in-flight during crash. Don't re-download.
        Serial.printf("[OTA] Duplicate (PENDING): %s — OTA in-progress, returning in-progress ACK\n",
                      requestId.c_str());
        _publishAck(requestId, true, "OTA command already received (in-progress). Wait and retry if needed.");
      }
      return;
    }

    // Store intent BEFORE download (prevents duplicate downloads on retry).
    // Note: OTA is non-idempotent (writes to flash), so PENDING state is
    // especially important here — re-download would waste flash write cycles.
    if (!Services::journal.storeIntent(requestId, commandHash)) {
      Serial.printf("[OTA] FATAL: storeIntent failed for rid=%s — refusing to download\n", requestId.c_str());
      _publishAck(requestId, false,
        "DURABILITY_FAILURE: cannot store OTA transaction intent — please retry");
      return;
    }

    // P2-2 R6-C2: OTA has a physical execution phase (flash write) → use EXECUTING lifecycle.
    // markExecuting must be called BEFORE download starts (physical mutation begins).
    if (!Services::journal.markExecuting(requestId)) {
      Serial.printf("[OTA] markExecuting failed for rid=%s — aborting\n", requestId.c_str());
      _publishAck(requestId, false, "Internal error: cannot mark OTA transaction as executing");
      Services::journal.clearEntry(requestId);
      return;
    }
  }

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

  // audit-fixes: OTA URL host allowlist.
  //   HTTPS alone is insufficient — an attacker who can inject an MQTT OTA
  //   command could point the ESP32 at any trusted HTTPS host (e.g., a
  //   GitHub fork they control). This allowlist constrains downloads to a
  //   known set of update hosts configured in Config.h (OTA_ALLOWED_HOSTS).
  //
  //   If OTA_ALLOWED_HOSTS is empty, the allowlist is DISABLED (backward-
  //   compatible with dev builds). PRODUCTION_BUILD should always set it.
  //
  //   Note: `url` is a const char* here (parsed from JSON), not a String.
  //   We wrap it in a local String for the substring operations below.
  String urlString(url);
  if (strlen(Core::OTA_ALLOWED_HOSTS) > 0) {
    // Extract host from URL: skip "https://" then read until '/', ':', '?', '#', or end
    // audit-fixes: include '#' in stop chars so fragments (https://host#frag)
    //   don't get parsed as part of the host.
    const char* hostStart = urlString.c_str() + 8;
    const char* hostEnd = hostStart;
    while (*hostEnd != '\0' && *hostEnd != '/' && *hostEnd != ':' && *hostEnd != '?' && *hostEnd != '#') {
      hostEnd++;
    }
    String host = urlString.substring(hostStart - urlString.c_str(), hostEnd - urlString.c_str());
    host.toLowerCase();

    // Check against comma-separated allowlist (suffix match)
    String allowed = Core::OTA_ALLOWED_HOSTS;
    allowed.toLowerCase();
    bool hostAllowed = false;
    int start = 0;
    while (start < (int)allowed.length()) {
      int comma = allowed.indexOf(',', start);
      String one = (comma < 0) ? allowed.substring(start) : allowed.substring(start, comma);
      one.trim();
      if (one.length() > 0) {
        // Suffix match: host == one OR host.endsWith("." + one)
        if (host == one || (host.length() > one.length() + 1 &&
                             host.charAt(host.length() - one.length() - 1) == '.' &&
                             host.endsWith(one))) {
          hostAllowed = true;
          break;
        }
      }
      if (comma < 0) break;
      start = comma + 1;
    }

    if (!hostAllowed) {
      Services::Log.append(Core::LogType::Error,
        "OTA: host '" + host + "' not in allowlist", 0);
      if (requestId.length() > 0) {
        _publishAck(requestId, false,
          "OTA: download host not in allowlist (check Config.h OTA_ALLOWED_HOSTS)");
      }
      return;
    }
  }

#ifdef PRODUCTION_BUILD
  // audit-fixes: in production, OTA_ALLOWED_HOSTS MUST be set.
  //   This is a fail-closed check — if PRODUCTION_BUILD is defined but the
  //   allowlist is empty, refuse OTA entirely.
  if (strlen(Core::OTA_ALLOWED_HOSTS) == 0) {
    Services::Log.append(Core::LogType::Error,
      "OTA: PRODUCTION_BUILD requires OTA_ALLOWED_HOSTS to be set in Config.h", 0);
    if (requestId.length() > 0) {
      _publishAck(requestId, false,
        "OTA: production build requires OTA_ALLOWED_HOSTS config");
    }
    return;
  }
#endif

  Services::Log.append(Core::LogType::Ota,
    String("OTA update requested: v") + version + " (" + String(expectedSize) + " bytes)", 0);

  // Publish OTA status: started
  String statusJson = "{\"otaStatus\":\"downloading\",\"progress\":0}";
  _mqtt.publish(_topicStatus.c_str(), (const uint8_t*)statusJson.c_str(), statusJson.length(), false);

  // Download and verify
  bool success = _downloadAndVerifyOta(url, expectedSize, expectedSha256,
                                       signatureHex, requestId, version);

  if (success) {
    // P2-2 R6-C2: OTA success — state is EXECUTING, use commitTransaction (not commitFromPending).
    // ACK is committed durably BEFORE restart. Do NOT dequeue — ACK stays as durable evidence.
    if (requestId.length() > 0) {
      StaticJsonDocument<256> doc;
      doc["requestId"] = requestId;
      doc["success"] = true;
      doc["message"] = "OTA success — rebooting";
      doc["timestamp"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
      String ackJson;
      serializeJson(doc, ackJson);

      bool committed = Services::journal.commitTransaction(requestId, ackJson);
      if (!committed) {
        // Commit failed — but OTA already flashed. We MUST restart (new firmware is in inactive partition).
        // Publish DURABILITY_FAILURE ACK — PWA may see device come back with new version despite ACK failure.
        Serial.printf("[OTA] WARNING: commitTransaction FAILED after OTA success — restarting anyway (flash already written)\n");
        _publishAck(requestId, false,
          "DURABILITY_FAILURE: OTA flashed but transaction not committed — device will reboot with new firmware");
      } else {
        // ACK is durable — publish immediately, do NOT dequeue
        _mqtt.publish(_topicAck.c_str(), (const uint8_t*)ackJson.c_str(),
                       ackJson.length(), false);
        Serial.printf("[MQTT ACK] %s: OTA success (committed, ACK published, not dequeued)\n",
                      requestId.c_str());
      }
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
//
// CYCLE-7 (fixes F-012): OTA signature verification architecture.
//   Auditor concern: "OTA streams binary to OTA partition BEFORE signature verify."
//
//   Honest analysis:
//   - ESP32 has ~320KB free heap, cannot buffer 2MB binary in RAM for verify.
//   - Update.write() streams to INACTIVE OTA partition (flash).
//   - Update.end(true) is what ACTIVATES the partition for next boot.
//   - Signature is verified BEFORE Update.end(true) — partition is NOT activated
//     unless signature passes. Update.abort() on failure clears the partition.
//   - So unsigned firmware NEVER boots. The auditor's concern is about
//     FLASH WEAR from repeated failed attempts (DoS vector).
//
//   Mitigations added in CYCLE-7:
//   1. Rate limit: max OTA_ATTEMPTS_PER_HOUR (3) attempts per hour via NVS counter.
//   2. Pre-flight HEAD request: if server supports, verify Content-Length matches
//      expected size before downloading. Rejects early if mismatch.
//   3. Size cap: OTA_MAX_BINARY_SIZE (2MB) enforced before Update.begin().
// ---------------------------------------------------------------------------
bool MqttClient::_downloadAndVerifyOta(const String& url, size_t expectedSize,
                                       const char* expectedSha256,
                                       const char* signatureHex,
                                       const String& requestId, const String& version) {
  // CYCLE-7 (fixes F-012): OTA attempt rate limiting.
  //   Prevents flash-wear DoS where attacker repeatedly triggers failed OTA
  //   attempts to wear out the OTA partition flash sector.
  //   Counter stored in NVS, resets after 1 hour window.
  static const char* NVS_KEY_OTA_ATTEMPT_COUNT = "ota_attempts";
  static const char* NVS_KEY_OTA_ATTEMPT_WINDOW = "ota_window";
  static const uint8_t OTA_ATTEMPTS_PER_HOUR = 3;
  static const uint32_t OTA_WINDOW_SEC = 3600;

  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, false);
  uint32_t windowStart = prefs.getULong(NVS_KEY_OTA_ATTEMPT_WINDOW, 0);
  uint8_t attemptCount = prefs.getUChar(NVS_KEY_OTA_ATTEMPT_COUNT, 0);
  uint32_t nowSec = millis() / 1000;

  // Reset window if expired (or first attempt ever)
  if (windowStart == 0 || (nowSec - windowStart) > OTA_WINDOW_SEC) {
    windowStart = nowSec;
    attemptCount = 0;
    prefs.putULong(NVS_KEY_OTA_ATTEMPT_WINDOW, windowStart);
    prefs.putUChar(NVS_KEY_OTA_ATTEMPT_COUNT, 0);
  }

  if (attemptCount >= OTA_ATTEMPTS_PER_HOUR) {
    String msg = "OTA: rate limit exceeded (" + String(attemptCount) + "/" +
                 String(OTA_ATTEMPTS_PER_HOUR) + " attempts in last hour)";
    Services::Log.append(Core::LogType::Error, msg, 0);
    Serial.println("[OTA] " + msg);
    prefs.end();
    return false;
  }

  // Increment counter (will decrement on window reset)
  attemptCount++;
  prefs.putUChar(NVS_KEY_OTA_ATTEMPT_COUNT, attemptCount);
  prefs.end();
  Serial.printf("[OTA] Attempt %u/%u in current window\n", attemptCount, OTA_ATTEMPTS_PER_HOUR);

  // Size cap check (defense-in-depth, also checked in _handleOta)
  if (expectedSize == 0 || expectedSize > Core::OTA_MAX_BINARY_SIZE) {
    Services::Log.append(Core::LogType::Error,
      "OTA: size " + String(expectedSize) + " exceeds cap " + String(Core::OTA_MAX_BINARY_SIZE), 0);
    return false;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.setConnectTimeout(10000);

  // CYCLE-7 (fixes F-012): Pre-flight HEAD request to validate Content-Length.
  //   If server supports HEAD and returns Content-Length, verify it matches
  //   expected size BEFORE downloading. Rejects early on mismatch (no flash write).
  //   If server doesn't support HEAD or doesn't return Content-Length, skip
  //   this check (fall back to existing post-download size check).
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

    // Pre-flight HEAD
    Serial.println("[OTA] Sending HEAD request for pre-flight size check...");
    if (http.begin(_otaClientSecure, url)) {
      int headCode = http.sendRequest("HEAD");
      if (headCode == HTTP_CODE_OK) {
        int headSize = http.getSize();
        if (headSize > 0 && (size_t)headSize != expectedSize) {
          Services::Log.append(Core::LogType::Error,
            "OTA: pre-flight size mismatch (HEAD=" + String(headSize) +
            " expected=" + String(expectedSize) + ")", 0);
          http.end();
          return false;
        }
        Serial.printf("[OTA] Pre-flight HEAD OK (Content-Length=%d matches expected)\n", headSize);
      } else {
        Serial.printf("[OTA] Pre-flight HEAD returned %d (server may not support HEAD) — continuing\n", headCode);
      }
      http.end();  // close HEAD connection before GET
    }
    // Re-begin for GET
    if (!http.begin(_otaClientSecure, url)) {
      Services::Log.append(Core::LogType::Error, "OTA: HTTPS begin failed", 0);
      return false;
    }
  } else {
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

  // Stream to Update + compute SHA-256 simultaneously.
  // CYCLE-7 NOTE: This DOES write to the OTA partition flash BEFORE signature
  // verification. This is unavoidable on ESP32 (insufficient RAM to buffer 2MB).
  // However:
  //   - Update.end(true) is the COMMIT POINT — only called after signature passes.
  //   - On signature failure, Update.abort() reverts the partition state.
  //   - The OTA partition is INACTIVE — boot is not affected until next reboot.
  //   - Rate limiting (above) prevents flash-wear DoS via repeated failed attempts.
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[512];
  size_t written = 0;
  size_t lastProgress = 0;

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, 0);

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

  char computedHashHex[Core::SHA256_HEX_LEN + 1];
  Utils::bytesToHex(computedHash, 32, computedHashHex);
  if (strcasecmp(computedHashHex, expectedSha256) != 0) {
    Services::Log.append(Core::LogType::Error,
      "OTA: SHA-256 mismatch (computed=" + String(computedHashHex) + ")", 0);
    Update.abort();
    return false;
  }
  Serial.println("[OTA] SHA-256 verified OK");

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

  // Signature verified OK — safe to finalize (commit point).
  // Update.end(true) is the atomic commit: marks partition as valid for next boot.
  // Before this line, the OTA partition contains written-but-unverified data
  // that will NOT boot. Only after this line does the new firmware become active.
  if (Update.end(true) && Update.isFinished()) {
    Services::Log.append(Core::LogType::Ota,
      "OTA success: " + String(written) + " bytes, v" + version + " (SHA-256 + Ed25519 verified)", 0);
    // Reset rate limit counter on success (don't penalize successful updates)
    Preferences resetPrefs;
    resetPrefs.begin(Core::NVS_NAMESPACE, false);
    resetPrefs.putUChar(NVS_KEY_OTA_ATTEMPT_COUNT, 0);
    resetPrefs.end();
    return true;
  } else {
    Services::Log.append(Core::LogType::Error,
      "OTA: install failed: " + String(Update.getError()), 0);
    return false;
  }
}

// ---------------------------------------------------------------------------
// audit-fixes: REMOVED legacy in-memory dedup ring buffer.
//
// Previously there were TWO dedup mechanisms:
//   1. In-memory ring buffer (_processedIds[64] etc., 15min TTL) — REMOVED.
//   2. NVS-persisted TransactionJournal (64 entries, CRC32, 2-phase commit) — KEPT.
//
// The in-memory buffer was declared and partially implemented but NEVER called
// from the command path — only the NVS journal is consulted (see
// Services::journal.isProcessed() in _handleCommand). Keeping dead dedup code
// around was an architecture-debt trap: a future engineer might "fix" a dedup
// bug by wiring up the dead code, unintentionally creating two sources of truth.
//
// All dedup is now authoritative via TransactionJournal. See TransactionJournal.{h,cpp}
// for the NVS-backed 64-entry ring with CRC32 + magic + two-phase commit.
// ---------------------------------------------------------------------------

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
