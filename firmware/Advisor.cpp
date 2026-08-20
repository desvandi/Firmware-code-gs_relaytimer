// =============================================================================
// AI/Advisor.cpp — GAS integration (ESP32 → Google Apps Script → Gemini)
// =============================================================================
#include "Advisor.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include "RtcDriver.h"
#include "WifiManager.h"
#include "PzemDriver.h"
#include "Crypto.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <esp_task_wdt.h>

namespace AI {

Advisor advisor;

void Advisor::begin(const String& gasUrl) {
  _gasUrl = gasUrl.length() > 0 ? gasUrl : String(Core::GAS_INSIGHTS_URL);
  if (_gasUrl.length() > 0) {
    Serial.printf("[AI] GAS URL configured: %s\n", _gasUrl.c_str());
    Serial.printf("[AI] Will POST logs every %lu minutes\n",
                  Core::GAS_POST_INTERVAL_MS / 60000UL);
  } else {
    Serial.println("[AI] GAS URL not configured — AI Insights disabled");
    Serial.println("[AI] To enable: deploy Code.gs, paste URL in Config.h");
  }
}

void Advisor::tick() {
  if (!isConfigured()) return;
  if (!TimerNet::wifi.isConnected()) return;
  if (_gasUrl.length() == 0) return;

  unsigned long now = millis();
  if (now - _lastSyncMs < Core::GAS_POST_INTERVAL_MS) return;

  // Time to sync
  Serial.println("[AI] Posting logs to GAS...");
  bool success = _postToGAS();
  _lastSyncMs = now;
  _lastSyncSuccess = success;

  if (success) {
    Serial.println("[AI] GAS sync successful");
    Services::Log.append(Core::LogType::ConfigChange, "AI insights synced to GAS", 0);
  } else {
    Serial.println("[AI] GAS sync failed");
    // Don't log error every time (too noisy) — only log if was previously success
    if (_lastSyncSuccess) {
      Services::Log.append(Core::LogType::Error, "GAS sync failed", 0);
    }
  }
}

String Advisor::_buildPayload() {
  // Build JSON payload: { mac, status: {...}, logs: [...] }
  DynamicJsonDocument doc(12288);  // Increased: channels + 50 logs + PZEM data

  // Use anonymous device ID (hash of MAC) instead of raw MAC for privacy
  // Gemini doesn't need to know the real MAC address
  String rawMac = TimerNet::wifi.getMacAddress();
  String anonymousId = Utils::sha256Hex(rawMac).substring(0, 16);
  doc["mac"] = anonymousId;
  Serial.printf("[AI] Anonymous device ID: %s (from MAC: %s)\n", anonymousId.c_str(), rawMac.c_str());

  // Status summary
  JsonObject status = doc.createNestedObject("status");
  status["firmwareVersion"] = Core::FIRMWARE_VERSION;
  status["deviceName"] = Core::deviceName;
  status["uptimeSeconds"] = (uint32_t)(millis() / 1000);
  status["currentTime"] = (uint64_t)Drivers::rtc.getUnixTime() * 1000ULL;
  status["timezone"] = Core::timezone;
  status["freeHeap"] = ESP.getFreeHeap();
  status["wifiRssi"] = TimerNet::wifi.getRssi();

  // PZEM power data (if available)
  if (Drivers::pzem.isAvailable()) {
    status["voltage"] = Drivers::pzem.getVoltage();
    status["current"] = Drivers::pzem.getCurrent();
    status["power"] = Drivers::pzem.getPower();
    status["energy"] = Drivers::pzem.getEnergy();
    status["frequency"] = Drivers::pzem.getFrequency();
    status["powerFactor"] = Drivers::pzem.getPowerFactor();
    status["apparentPower"] = Drivers::pzem.getApparentPower();
    status["reactivePower"] = Drivers::pzem.getReactivePower();
    status["energyToday"] = Drivers::pzem.getEnergyToday();
    status["powerMax"] = Drivers::pzem.getPowerMax();
    status["powerAvg"] = Drivers::pzem.getPowerAvg();
  }

  // Channels summary (compact)
  JsonArray channels = status.createNestedArray("channels");
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    JsonObject ch = channels.createNestedObject();
    ch["id"] = i + 1;
    ch["name"] = Core::channels[i].name;
    ch["state"] = Core::relayState[i];
    ch["modeAuto"] = Core::channels[i].modeAuto;
    ch["energyWh"] = Core::channels[i].energyWh;
    ch["wattage"] = Core::channels[i].wattage;
    const char* srcStr =
      Core::relaySource[i] == Core::RelaySource::Manual ? "manual" :
      Core::relaySource[i] == Core::RelaySource::Schedule ? "schedule" :
      Core::relaySource[i] == Core::RelaySource::Pir ? "pir" : "off";
    ch["source"] = srcStr;
  }

  // Recent logs — read last 50 entries from activity log file
  JsonArray logs = doc.createNestedArray("logs");
  if (LittleFS.exists(Core::PATH_ACTIVITY_LOG)) {
    File f = LittleFS.open(Core::PATH_ACTIVITY_LOG, "r");
    if (f) {
      // Read file from end, collect last 50 lines
      // LittleFS doesn't support seek-from-end well, so read all and take last 50
      std::vector<String> lines;
      String line;
      while (f.available() && lines.size() < 200) {
        line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) lines.push_back(line);
        // Reset watchdog periodically during file read
        if (lines.size() % 50 == 0) esp_task_wdt_reset();
      }
      f.close();

      // Take last 50 lines
      int start = lines.size() > 50 ? lines.size() - 50 : 0;
      for (int i = start; i < (int)lines.size(); i++) {
        StaticJsonDocument<256> logEntry;
        DeserializationError err = deserializeJson(logEntry, lines[i]);
        if (!err) {
          JsonObject logObj = logs.createNestedObject();
          logObj["id"] = logEntry["id"] | 0;
          logObj["timestamp"] = (uint64_t)(logEntry["ts"] | 0) * 1000ULL;
          logObj["type"] = logEntry["type"] | "";
          logObj["channelId"] = logEntry["ch"] | 0;
          logObj["message"] = logEntry["msg"] | "";
        }
      }
    }
  }

  String payload;
  serializeJson(doc, payload);
  return payload;
}

bool Advisor::_postToGAS() {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  http.setConnectTimeout(5000);

  String gasSecretHex = TimerNet::wifi.getGasSecretHex();
  if (gasSecretHex.length() != Core::GAS_SECRET_LEN * 2) {
    Serial.println("[AI] ERROR: GAS secret not configured — skipping POST");
    return false;
  }

  String payload = _buildPayload();
  Serial.printf("[AI] POST payload size: %d bytes\n", payload.length());

  String urlWithAuth = _buildAuthenticatedUrl("POST", payload);
  if (urlWithAuth.length() == 0) {
    Serial.println("[AI] Failed to build authenticated URL — skipping POST");
    return false;
  }

  if (!http.begin(urlWithAuth)) {
    Serial.println("[AI] HTTP begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  esp_task_wdt_reset();
  int httpCode = http.POST(payload);
  esp_task_wdt_reset();

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
    String response = http.getString();
    Serial.printf("[AI] GAS response: %s\n", response.substring(0, 200).c_str());
    http.end();
    return true;
  }

  Serial.printf("[AI] GAS HTTP error: %d\n", httpCode);
  http.end();
  return false;
}

// Phase B: Fetch insights from GAS via authenticated GET.
String Advisor::fetchInsights() {
  if (!isConfigured()) {
    Serial.println("[Insights] GAS not configured — returning mock");
    return "{\"success\":true,\"insights\":[{\"id\":\"mock-not-configured\","
           "\"category\":\"habit_analysis\",\"severity\":\"info\","
           "\"title\":\"AI Insights not configured\",\"body\":\"Deploy Code.gs "
           "and set GAS_INSIGHTS_URL in firmware Config.h.\",\"channelId\":null,"
           "\"action\":{\"label\":\"Dismiss\",\"type\":\"dismiss\"},"
           "\"generatedAt\":0,\"source\":\"mock\",\"advisoryOnly\":true}],"
           "\"mock\":true,\"message\":\"GAS not configured\"}";
  }

  if (!TimerNet::wifi.isConnected()) {
    Serial.println("[Insights] WiFi not connected — returning mock");
    return "{\"success\":true,\"insights\":[{\"id\":\"mock-offline\","
           "\"category\":\"fault_detection\",\"severity\":\"warning\","
           "\"title\":\"Device offline\",\"body\":\"WiFi not connected.\",\"channelId\":null,"
           "\"action\":{\"label\":\"Dismiss\",\"type\":\"dismiss\"},"
           "\"generatedAt\":0,\"source\":\"mock\",\"advisoryOnly\":true}],"
           "\"mock\":true,\"message\":\"WiFi not connected\"}";
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  http.setConnectTimeout(5000);

  String urlWithAuth = _buildAuthenticatedUrl("GET", "");
  if (urlWithAuth.length() == 0) {
    return "{\"success\":false,\"error\":\"auth_url_build_failed\",\"insights\":[],\"mock\":true}";
  }

  if (!http.begin(urlWithAuth)) {
    return "{\"success\":false,\"error\":\"http_begin_failed\",\"insights\":[],\"mock\":true}";
  }

  Serial.println("[Insights] Fetching insights from GAS (GET, HMAC-authenticated)...");
  esp_task_wdt_reset();
  int httpCode = http.GET();
  esp_task_wdt_reset();

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
    String response = http.getString();
    Serial.printf("[Insights] GAS response (%d bytes): %.200s\n",
                  response.length(), response.c_str());
    http.end();
    return response;
  }

  Serial.printf("[Insights] GAS HTTP error: %d — returning mock\n", httpCode);
  http.end();
  String err = "{\"success\":false,\"error\":\"http_";
  err += String(httpCode);
  err += "\",\"insights\":[{\"id\":\"mock-http-error\",\"category\":\"fault_detection\","
         "\"severity\":\"info\",\"title\":\"AI Insights temporarily unavailable\","
         "\"body\":\"GAS returned HTTP error. Relay control unaffected.\","
         "\"channelId\":null,\"action\":{\"label\":\"Dismiss\",\"type\":\"dismiss\"},"
         "\"generatedAt\":0,\"source\":\"mock\",\"advisoryOnly\":true}],"
         "\"mock\":true}";
  return err;
}

// Phase B: Build GAS URL with HMAC auth query parameters.
String Advisor::_buildAuthenticatedUrl(const String& method, const String& body) {
  String gasSecretHex = TimerNet::wifi.getGasSecretHex();
  if (gasSecretHex.length() != Core::GAS_SECRET_LEN * 2) {
    Serial.println("[AI] GAS secret not configured — cannot build auth URL");
    return "";
  }

  uint8_t secret[Core::GAS_SECRET_LEN];
  Utils::hexToBytes(gasSecretHex.c_str(), secret, Core::GAS_SECRET_LEN);

  uint32_t timestamp = (uint32_t)Drivers::rtc.getUnixTime();
  String nonce = Utils::generateToken(16);
  String anonId = Utils::sha256Hex(TimerNet::wifi.getMacAddress()).substring(0, 16);

  String canonical = method + "\n" + String(timestamp) + "\n" + nonce + "\n" + anonId + "\n" + body;

  uint8_t hmac[32];
  Utils::hmacSha256(secret, Core::GAS_SECRET_LEN,
                    (const uint8_t*)canonical.c_str(), canonical.length(),
                    hmac);
  char hmacHex[65];
  Utils::bytesToHex(hmac, 32, hmacHex);
  memset(secret, 0, sizeof(secret));
  memset(hmac, 0, sizeof(hmac));

  String urlWithAuth = _gasUrl;
  urlWithAuth += (urlWithAuth.indexOf('?') >= 0 ? "&" : "?");
  urlWithAuth += "deviceId=" + anonId;
  urlWithAuth += "&timestamp=" + String(timestamp);
  urlWithAuth += "&nonce=" + nonce;
  urlWithAuth += "&signature=" + String(hmacHex);

  Serial.printf("[AI] HMAC signed (%s): ts=%u nonce=%s sig=%.16s...\n",
                method.c_str(), timestamp, nonce.c_str(), hmacHex);

  return urlWithAuth;
}

} // namespace AI
