// =============================================================================
// Storage/ConfigStore.cpp
// =============================================================================
#include "ConfigStore.h"
#include "FileSystem.h"
#include "Globals.h"
#include "Config.h"
#include "Json.h"
#include "Crypto.h"
#include "Crc.h"
#include "LogService.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

namespace Storage {

ConfigStore config;

// ============================================================================
// USER CONFIG
// ============================================================================
void ConfigStore::initDefaultUserConfig() {
  uint64_t mac = ESP.getEfuseMac();
  char defaultPass[17];
  snprintf(defaultPass, sizeof(defaultPass), "T%04X%04X%04X",
           (uint16_t)(mac >> 32), (uint16_t)(mac >> 16), (uint16_t)mac);
  strcpy(Core::userConfig.wwwUser, "admin");
  Utils::generateRandomBytes(Core::userConfig.salt, Core::SALT_LEN);
  Core::userConfig.iterations = Core::PBKDF2_ITERATIONS;
  uint8_t hash[32];
  if (!Utils::pbkdf2HmacSha256(defaultPass, strlen(defaultPass),
                               Core::userConfig.salt, Core::SALT_LEN,
                               Core::userConfig.iterations, hash)) {
    memset(Core::userConfig.passHashHex, 0, sizeof(Core::userConfig.passHashHex));
    return;
  }
  Utils::bytesToHex(hash, 32, Core::userConfig.passHashHex);
  Services::Log.append(Core::LogType::ConfigChange, "Default user config created", 0);
  Serial.println(F("========================================"));
  Serial.println(F("DEFAULT PASSWORD (catat dan amankan!):"));
  Serial.println(defaultPass);
  Serial.println(F("========================================"));
}

void ConfigStore::loadUserConfig() {
  if (!Storage::fs.exists(Core::PATH_CONFIG_JSON)) {
    if (!Storage::fs.exists(Core::PATH_CONFIG_BAK)) {
      initDefaultUserConfig();
      saveUserConfig();
      return;
    }
    Storage::fs.rename(Core::PATH_CONFIG_BAK, Core::PATH_CONFIG_JSON);
  }
  String content = Storage::fs.readAll(Core::PATH_CONFIG_JSON);
  if (content.length() == 0) {
    initDefaultUserConfig();
    saveUserConfig();
    return;
  }
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, content);
  if (err) {
    initDefaultUserConfig();
    saveUserConfig();
    return;
  }
  uint32_t storedCRC = doc["_crc"] | 0;
  uint32_t calcCRC = Utils::computeDocCRC(doc);
  if (storedCRC != calcCRC) {
    initDefaultUserConfig();
    saveUserConfig();
    return;
  }
  if (doc.containsKey("user")) {
    const char* u = doc["user"];
    if (u) {
      strncpy(Core::userConfig.wwwUser, u, Core::MAX_USER_LEN);
      Core::userConfig.wwwUser[Core::MAX_USER_LEN] = '\0';
    }
  }
  if (doc.containsKey("passhash")) {
    const char* h = doc["passhash"];
    if (h && strlen(h) == Core::HASH_HEX_LEN) {
      strncpy(Core::userConfig.passHashHex, h, Core::HASH_HEX_LEN);
      Core::userConfig.passHashHex[Core::HASH_HEX_LEN] = '\0';
    }
  }
  if (doc.containsKey("salt")) {
    const char* s = doc["salt"];
    if (s && strlen(s) == Core::SALT_LEN * 2) {
      Utils::hexToBytes(s, Core::userConfig.salt, Core::SALT_LEN);
    }
  }
  if (doc.containsKey("iterations")) {
    Core::userConfig.iterations = doc["iterations"] | Core::PBKDF2_ITERATIONS;
    if (Core::userConfig.iterations < 1000) Core::userConfig.iterations = Core::PBKDF2_ITERATIONS;
  }
}

void ConfigStore::saveUserConfig() {
  StaticJsonDocument<1024> doc;
  doc["user"] = Core::userConfig.wwwUser;
  doc["passhash"] = Core::userConfig.passHashHex;
  char saltHex[Core::SALT_LEN * 2 + 1] = {0};
  Utils::bytesToHex(Core::userConfig.salt, Core::SALT_LEN, saltHex);
  doc["salt"] = saltHex;
  doc["iterations"] = Core::userConfig.iterations;
  Utils::appendCRC(doc);
  String out;
  serializeJson(doc, out);
  if (Storage::fs.atomicWrite(Core::PATH_CONFIG_JSON, out)) {
    Services::Log.append(Core::LogType::ConfigChange, "User config saved", 0);
  } else {
    Services::Log.append(Core::LogType::Error, "Failed to save user config", 0);
  }
}

// ============================================================================
// SCHEDULE / CHANNEL CONFIG
// ============================================================================
void ConfigStore::resetChannels() {
  for (int i = 0; i < Core::NUM_CHANNELS; i++) {
    snprintf(Core::channels[i].name, Core::MAX_NAME_BUF, "Relay %d", i + 1);
    Core::channels[i].schedCount = 0;
    Core::channels[i].manualState = false;
    Core::channels[i].modeAuto = true;
    Core::channels[i].pirEnabled = false;
    Core::channels[i].pirHoldTime = 180;
    Core::channels[i].energyWh = 0;
    Core::channels[i].wattage = 10;
    Core::channels[i].lastOnMs = 0;
    for (int j = 0; j < Core::MAX_SCHEDULES; j++) {
      Core::channels[i].sched[j].onTime[0] = '\0';
      Core::channels[i].sched[j].offTime[0] = '\0';
      Core::channels[i].sched[j].onMin = 0;
      Core::channels[i].sched[j].offMin = 0;
      Core::channels[i].sched[j].dayMask = 0;
      Core::channels[i].sched[j].enabled = true;
    }
  }
}

void ConfigStore::loadSchedule() {
  resetChannels();
  auto loadFromFile = [](const char* path) -> bool {
    if (!Storage::fs.exists(path)) return false;
    String content = Storage::fs.readAll(path);
    if (content.length() == 0) return false;
    StaticJsonDocument<16384> doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) return false;
    uint32_t storedCRC = doc["_crc"] | 0;
    uint32_t calcCRC = Utils::computeDocCRC(doc);
    if (storedCRC != calcCRC) return false;
    JsonArray arr = doc["channels"].as<JsonArray>();
    if (!arr || arr.size() != Core::NUM_CHANNELS) return false;
    for (int i = 0; i < Core::NUM_CHANNELS; i++) {
      JsonObject ch = arr[i];
      if (ch.containsKey("name")) {
        const char* n = ch["name"];
        if (n) {
          strncpy(Core::channels[i].name, n, Core::MAX_NAME_LEN);
          Core::channels[i].name[Core::MAX_NAME_LEN] = '\0';
        }
      }
      if (ch.containsKey("modeAuto") && ch["modeAuto"].is<bool>())
        Core::channels[i].modeAuto = ch["modeAuto"].as<bool>();
      if (ch.containsKey("manualState") && ch["manualState"].is<bool>())
        Core::channels[i].manualState = ch["manualState"].as<bool>();
      if (ch.containsKey("pirEnabled") && ch["pirEnabled"].is<bool>())
        Core::channels[i].pirEnabled = ch["pirEnabled"].as<bool>();
      if (ch.containsKey("pirHoldTime")) {
        uint16_t ht = ch["pirHoldTime"].as<uint16_t>();
        if (ht < 1) ht = 1;
        if (ht > 3600) ht = 3600;
        Core::channels[i].pirHoldTime = ht;
      }
      if (ch.containsKey("schedules")) {
        JsonArray schedArr = ch["schedules"];
        int count = 0;
        for (JsonObject entry : schedArr) {
          if (count >= Core::MAX_SCHEDULES) break;
          const char* onStr = entry["on"] ;
          const char* offStr = entry["off"];
          uint8_t day = entry["day"] | 0;
          bool en = entry["enabled"] | true;
          if (onStr && offStr && strlen(onStr)==5 && strlen(offStr)==5) {
            uint16_t onMin, offMin;
            if (Utils::parseMinutes(onStr, onMin) && Utils::parseMinutes(offStr, offMin)) {
              strncpy(Core::channels[i].sched[count].onTime, onStr, 5);
              Core::channels[i].sched[count].onTime[5] = '\0';
              strncpy(Core::channels[i].sched[count].offTime, offStr, 5);
              Core::channels[i].sched[count].offTime[5] = '\0';
              Core::channels[i].sched[count].onMin = onMin;
              Core::channels[i].sched[count].offMin = offMin;
              Core::channels[i].sched[count].dayMask = day & 0x7F;
              Core::channels[i].sched[count].enabled = en;
              count++;
            }
          }
        }
        Core::channels[i].schedCount = count;
      }
    }
    return true;
  };
  if (loadFromFile(Core::PATH_SCHEDULE_JSON)) {
    Services::Log.append(Core::LogType::ConfigChange, "Schedule loaded", 0);
  } else if (loadFromFile(Core::PATH_SCHEDULE_BAK)) {
    Services::Log.append(Core::LogType::ConfigChange, "Schedule loaded from backup", 0);
  } else {
    saveSchedule(true);
  }
}

void ConfigStore::saveSchedule(bool force) {
  if (!force && !Core::scheduleDirty) return;
  StaticJsonDocument<16384> doc;
  doc["configVersion"] = Core::CONFIG_VERSION;
  JsonArray arr = doc.createNestedArray("channels");
  for (int i = 0; i < Core::NUM_CHANNELS; i++) {
    JsonObject ch = arr.createNestedObject();
    ch["name"] = Core::channels[i].name;
    ch["modeAuto"] = Core::channels[i].modeAuto;
    ch["manualState"] = Core::channels[i].manualState;
    ch["pirEnabled"] = Core::channels[i].pirEnabled;
    ch["pirHoldTime"] = Core::channels[i].pirHoldTime;
    JsonArray schedArr = ch.createNestedArray("schedules");
    for (int j = 0; j < Core::channels[i].schedCount; j++) {
      JsonObject entry = schedArr.createNestedObject();
      entry["on"] = Core::channels[i].sched[j].onTime;
      entry["off"] = Core::channels[i].sched[j].offTime;
      entry["day"] = Core::channels[i].sched[j].dayMask;
      entry["enabled"] = Core::channels[i].sched[j].enabled;
    }
  }
  Utils::appendCRC(doc);
  String out;
  serializeJson(doc, out);
  if (Storage::fs.atomicWrite(Core::PATH_SCHEDULE_JSON, out)) {
    Core::scheduleDirty = false;
    Core::firstDirtySet = false;
  } else {
    Core::scheduleDirty = true;
    Core::lastSaveTime = millis() - Core::SAVE_DELAY_MS + 5000;
  }
  esp_task_wdt_reset();
}

void ConfigStore::markDirty() {
  Core::scheduleDirty = true;
  Core::lastSaveTime = millis();
  if (!Core::firstDirtySet) {
    Core::firstDirtyTime = millis();
    Core::firstDirtySet = true;
  }
}

void ConfigStore::clearDirty() {
  Core::scheduleDirty = false;
  Core::firstDirtySet = false;
  Core::lastSaveTime = millis();
}

// ============================================================================
// DEVICE CONFIG (name, timezone) — stored in NVS via Preferences
// ============================================================================
void ConfigStore::loadDeviceConfig() {
  // Use Preferences (NVS) for small key-value pairs (name, timezone, jwt secret)
  Preferences prefs;
  prefs.begin("timer12", true);
  const char* defaultName = "Timer12-ESP32";
  strncpy(Core::deviceName, prefs.getString("name", defaultName).c_str(), 32);
  Core::deviceName[32] = '\0';
  strncpy(Core::timezone, prefs.getString("tz", Core::DEFAULT_TIMEZONE).c_str(), 39);
  Core::timezone[39] = '\0';
  strncpy(Core::jwtSecret, prefs.getString("jwt", Core::JWT_SECRET_DEFAULT).c_str(), 64);
  Core::jwtSecret[64] = '\0';
  prefs.end();
}

void ConfigStore::saveDeviceConfig() {
  Preferences prefs;
  prefs.begin("timer12", false);
  prefs.putString("name", Core::deviceName);
  prefs.putString("tz", Core::timezone);
  // JWT secret is normally only set once at factory; allow override here for dev
  prefs.end();
  Services::Log.append(Core::LogType::ConfigChange, "Device config saved", 0);
}

// ============================================================================
// EXPORT / IMPORT (full backup — channels + schedules + device info)
// ============================================================================
String ConfigStore::exportAll() {
  StaticJsonDocument<16384> doc;
  doc["deviceName"] = Core::deviceName;
  doc["timezone"] = Core::timezone;
  doc["firmwareVersion"] = Core::FIRMWARE_VERSION;
  JsonArray arr = doc.createNestedArray("channels");
  for (int i = 0; i < Core::NUM_CHANNELS; i++) {
    JsonObject ch = arr.createNestedObject();
    ch["name"] = Core::channels[i].name;
    ch["modeAuto"] = Core::channels[i].modeAuto;
    ch["manualState"] = Core::channels[i].manualState;
    ch["pirEnabled"] = Core::channels[i].pirEnabled;
    ch["pirHoldTime"] = Core::channels[i].pirHoldTime;
    JsonArray schedArr = ch.createNestedArray("schedules");
    for (int j = 0; j < Core::channels[i].schedCount; j++) {
      JsonObject entry = schedArr.createNestedObject();
      entry["on"] = Core::channels[i].sched[j].onTime;
      entry["off"] = Core::channels[i].sched[j].offTime;
      entry["day"] = Core::channels[i].sched[j].dayMask;
      entry["enabled"] = Core::channels[i].sched[j].enabled;
    }
  }
  Utils::appendCRC(doc);
  String out;
  serializeJson(doc, out);
  return out;
}

bool ConfigStore::importAll(const String& json) {
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, json);
  if (err) return false;
  JsonArray arr = doc["channels"].as<JsonArray>();
  if (!arr || arr.size() != Core::NUM_CHANNELS) return false;

  Core::Channel tempChannels[Core::NUM_CHANNELS];
  for (int i = 0; i < Core::NUM_CHANNELS; i++) {
    strcpy(tempChannels[i].name, "Relay");
    tempChannels[i].schedCount = 0;
    tempChannels[i].manualState = false;
    tempChannels[i].modeAuto = true;
    tempChannels[i].pirEnabled = false;
    tempChannels[i].pirHoldTime = 180;
    tempChannels[i].energyWh = 0;
    tempChannels[i].wattage = 10;
    tempChannels[i].lastOnMs = 0;
    for (int j = 0; j < Core::MAX_SCHEDULES; j++) {
      tempChannels[i].sched[j].onTime[0] = '\0';
      tempChannels[i].sched[j].offTime[0] = '\0';
    }
  }
  for (int i = 0; i < Core::NUM_CHANNELS; i++) {
    JsonObject ch = arr[i];
    const char* n = ch["name"] | "Relay";
    strncpy(tempChannels[i].name, n, Core::MAX_NAME_LEN);
    tempChannels[i].name[Core::MAX_NAME_LEN] = '\0';
    tempChannels[i].modeAuto = ch["modeAuto"] | true;
    tempChannels[i].manualState = ch["manualState"] | false;
    tempChannels[i].pirEnabled = ch["pirEnabled"] | false;
    tempChannels[i].pirHoldTime = ch["pirHoldTime"] | 180;
    if (tempChannels[i].pirHoldTime < 1) tempChannels[i].pirHoldTime = 1;
    if (tempChannels[i].pirHoldTime > 3600) tempChannels[i].pirHoldTime = 3600;
    if (ch.containsKey("schedules")) {
      JsonArray schedArr = ch["schedules"];
      int count = 0;
      for (JsonObject entry : schedArr) {
        if (count >= Core::MAX_SCHEDULES) break;
        const char* on = entry["on"];
        const char* off = entry["off"];
        if (!on || !off || strlen(on) != 5 || strlen(off) != 5) continue;
        uint16_t onMin, offMin;
        if (!Utils::parseMinutes(on, onMin) || !Utils::parseMinutes(off, offMin)) continue;
        if (onMin == offMin) continue;
        strncpy(tempChannels[i].sched[count].onTime, on, 5);
        tempChannels[i].sched[count].onTime[5] = '\0';
        strncpy(tempChannels[i].sched[count].offTime, off, 5);
        tempChannels[i].sched[count].offTime[5] = '\0';
        tempChannels[i].sched[count].onMin = onMin;
        tempChannels[i].sched[count].offMin = offMin;
        tempChannels[i].sched[count].dayMask = (uint8_t)(entry["day"] | 0) & 0x7F;
        tempChannels[i].sched[count].enabled = entry["enabled"] | true;
        count++;
      }
      tempChannels[i].schedCount = count;
    }
  }
  for (int i = 0; i < Core::NUM_CHANNELS; i++) Core::channels[i] = tempChannels[i];

  if (doc.containsKey("deviceName")) {
    const char* dn = doc["deviceName"];
    if (dn) {
      strncpy(Core::deviceName, dn, 32);
      Core::deviceName[32] = '\0';
    }
  }
  if (doc.containsKey("timezone")) {
    const char* tz = doc["timezone"];
    if (tz) {
      strncpy(Core::timezone, tz, 39);
      Core::timezone[39] = '\0';
    }
  }

  saveSchedule(true);
  saveDeviceConfig();
  Services::Log.append(Core::LogType::ConfigChange, "Configuration imported", 0);
  return true;
}

} // namespace Storage
