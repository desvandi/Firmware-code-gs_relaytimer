// =============================================================================
// WifiManager.cpp — STA mode + Config Portal + AP fallback
// =============================================================================
#include "WifiManager.h"
#include "Config.h"
#include "Globals.h"
#include "LogService.h"
#include "Crypto.h"  // audit-fixes: Utils::sha256Hex / generateRandomBytes / bytesToHex live here
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <WebServer.h>

namespace TimerNet {

WifiManager wifi;

// Config Portal HTML (mobile-friendly, dark theme matching PWA)
// audit-fixes: REMOVED MQTT password + device PIN from the HTML.
//   Displaying these on an open AP page made them trivially extractable by
//   anyone within WiFi range during the brief config-portal window. The
//   legitimate owner has physical access and can read them from the Serial
//   Monitor (in dev mode) or from a printed provisioning sheet (in production).
static const char CONFIG_PORTAL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Timer12 Setup</title>
<style>
  body{font-family:sans-serif;background:#0F172A;color:#E2E8F0;margin:0;padding:20px;display:flex;justify-content:center;align-items:center;min-height:100vh}
  .card{background:#1E293B;border:1px solid #334155;border-radius:12px;padding:24px;max-width:400px;width:100%}
  h1{font-size:20px;margin:0 0 8px;color:#10B981}
  p{font-size:13px;color:#94A3B8;margin:0 0 20px}
  label{display:block;font-size:12px;color:#94A3B8;margin-bottom:4px}
  input{width:100%;padding:12px;margin-bottom:16px;background:#0F172A;border:1px solid #334155;border-radius:8px;color:#E2E8F0;font-size:14px;box-sizing:border-box}
  button{width:100%;padding:14px;background:#10B981;color:white;border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer}
  button:hover{background:#059669}
  .info{margin-top:16px;padding:12px;background:#0F172A;border-radius:8px;font-size:11px;color:#64748B;font-family:monospace}
</style>
</head><body>
<div class="card">
  <h1>Timer12 Setup</h1>
  <p>Enter your WiFi credentials to connect this device.</p>
  <form action="/save" method="POST">
    <label>WiFi SSID</label>
    <input name="ssid" placeholder="Your WiFi name" required>
    <label>WiFi Password</label>
    <input name="pass" type="password" placeholder="Your WiFi password">
    <button type="submit">Save & Connect</button>
  </form>
  <div class="info">
    Device MAC: __MAC__<br>
    After connecting, see Serial Monitor (dev) or provisioning sheet (prod) for MQTT credentials.
  </div>
</div>
</body></html>
)HTML";

void WifiManager::generateApPassword() {
  // audit-fixes: AP password was previously MAC-derived (`AP-<mac-hex>`),
  //   making it predictable from WiFi beacon frames. Now generates a random
  //   12-char alphanumeric password (CSPRNG). Stored in NVS so survives reboot.
  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, false);
  String stored = prefs.getString("ap_pass", "");
  if (stored.length() >= 8) {
    strncpy(_apPassword, stored.c_str(), sizeof(_apPassword) - 1);
    _apPassword[sizeof(_apPassword) - 1] = '\0';
  } else {
    // Generate random 12-char alphanumeric password (CSPRNG via esp_random)
    static const char charset[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // no I,O,0,1
    for (uint8_t i = 0; i < 12; i++) {
      _apPassword[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    _apPassword[12] = '\0';
    prefs.putString("ap_pass", _apPassword);
  }
  prefs.end();
  strncpy(Core::apPassword, _apPassword, 32);
  Core::apPassword[32] = '\0';
}

String WifiManager::getMacAddress() const {
  uint64_t mac = ESP.getEfuseMac();
  char macStr[13];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           (uint8_t)(mac >> 40), (uint8_t)(mac >> 32),
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 16),
           (uint8_t)(mac >> 8),  (uint8_t)mac);
  return String(macStr);
}

void WifiManager::_generateMqttPassword() {
  // Generate random 8-char alphanumeric password
  static const char charset[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // no I,O,0,1 (ambiguous)
  for (uint8_t i = 0; i < Core::MQTT_PASSWORD_LEN; i++) {
    _mqttPassword[i] = charset[esp_random() % (sizeof(charset) - 1)];
  }
  _mqttPassword[Core::MQTT_PASSWORD_LEN] = '\0';
}

void WifiManager::_generateDevicePin() {
  // Generate random 6-digit PIN
  uint32_t pin = esp_random() % 1000000;
  snprintf(_devicePin, sizeof(_devicePin), "%06u", pin);
}

void WifiManager::_loadCredentials() {
  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, true);

  // Load WiFi creds
  String ssid = prefs.getString(Core::NVS_KEY_WIFI_SSID, "");
  String pass = prefs.getString(Core::NVS_KEY_WIFI_PASS, "");
  if (ssid.length() > 0) {
    // Store in global for _tryStaMode
    strncpy(Core::apPassword, ssid.c_str(), 32);  // reuse apPassword field temporarily
    // We'll use a different approach: store in static vars
  }

  // Load or generate MQTT password
  String mqttPass = prefs.getString(Core::NVS_KEY_MQTT_PASS, "");
  if (mqttPass.length() == Core::MQTT_PASSWORD_LEN) {
    strncpy(_mqttPassword, mqttPass.c_str(), Core::MQTT_PASSWORD_LEN);
    _mqttPassword[Core::MQTT_PASSWORD_LEN] = '\0';
  } else {
    _generateMqttPassword();
    prefs.end();
    prefs.begin(Core::NVS_NAMESPACE, false);
    prefs.putString(Core::NVS_KEY_MQTT_PASS, _mqttPassword);
  }

  // Load or generate device PIN
  String pin = prefs.getString(Core::NVS_KEY_DEVICE_PIN, "");
  if (pin.length() == 6) {
    strncpy(_devicePin, pin.c_str(), 6);
    _devicePin[6] = '\0';
  } else {
    _generateDevicePin();
    prefs.putString(Core::NVS_KEY_DEVICE_PIN, _devicePin);
  }

  // P0 #7 (audit round 9): Load or generate GAS HMAC secret (32 bytes).
  // Used to sign hourly POST to Google Apps Script. User copies the hex
  // secret to GAS Script Properties as: DEVICE_<anonymousId>_SECRET
  String gasSecretHex = prefs.getString(Core::NVS_KEY_GAS_SECRET, "");
  if (gasSecretHex.length() == Core::GAS_SECRET_LEN * 2) {
    _gasSecretHex = gasSecretHex;
  } else {
    // Generate 32 random bytes → 64 hex chars
    uint8_t secret[Core::GAS_SECRET_LEN];
    Utils::generateRandomBytes(secret, Core::GAS_SECRET_LEN);
    char hex[Core::GAS_SECRET_LEN * 2 + 1];
    Utils::bytesToHex(secret, Core::GAS_SECRET_LEN, hex);
    _gasSecretHex = String(hex);
    memset(secret, 0, sizeof(secret));
    prefs.putString(Core::NVS_KEY_GAS_SECRET, _gasSecretHex);
    Serial.println("[WiFi] Generated new GAS HMAC secret (first-time setup)");
  }

  prefs.end();

#ifdef PRODUCTION_BUILD
  // audit-fixes: do NOT print secrets to Serial in production.
  //   Anyone with USB access during boot could read them. In production,
  //   the legitimate owner already has a printed provisioning sheet (or
  //   reads NVS via authenticated API). Log only that secrets exist.
  Serial.println("[WiFi] MQTT password: [REDACTED in PRODUCTION_BUILD]");
  Serial.println("[WiFi] Device PIN:     [REDACTED in PRODUCTION_BUILD]");
  Serial.println("[WiFi] GAS HMAC secret: [REDACTED in PRODUCTION_BUILD]");
  String anonId = Utils::sha256Hex(getMacAddress()).substring(0, 16);
  Serial.printf("[WiFi] GAS Script Property key: DEVICE_%s_SECRET\n", anonId.c_str());
  Serial.println("[WiFi] (Get secret value from NVS via authenticated /api/config or provisioning sheet)");
#else
  Serial.printf("[WiFi] MQTT Password: %s\n", _mqttPassword);
  Serial.printf("[WiFi] Device PIN: %s\n", _devicePin);
  Serial.printf("[WiFi] GAS HMAC Secret: %s\n", _gasSecretHex.c_str());
  Serial.println("[WiFi] Copy GAS secret to GAS Script Properties:");
  String anonId = Utils::sha256Hex(getMacAddress()).substring(0, 16);
  Serial.printf("[WiFi]   Key: DEVICE_%s_SECRET\n", anonId.c_str());
  Serial.printf("[WiFi]   Value: %s\n", _gasSecretHex.c_str());
#endif
}

void WifiManager::_saveCredentials(const String& ssid, const String& password) {
  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, false);
  prefs.putString(Core::NVS_KEY_WIFI_SSID, ssid);
  prefs.putString(Core::NVS_KEY_WIFI_PASS, password);
  prefs.end();
  Serial.printf("[WiFi] Saved credentials: SSID=%s\n", ssid.c_str());
}

bool WifiManager::_tryStaMode() {
  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, true);
  String ssid = prefs.getString(Core::NVS_KEY_WIFI_SSID, "");
  String pass = prefs.getString(Core::NVS_KEY_WIFI_PASS, "");
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println("[WiFi] No stored SSID — opening Config Portal");
    return false;
  }

  Serial.printf("[WiFi] Connecting to \"%s\"...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower((wifi_power_t)Core::WIFI_TX_POWER_DBM);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < Core::WIFI_STA_MAX_RETRIES) {
    unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - startMs) < Core::WIFI_STA_TIMEOUT_MS) {
      delay(500);
      Serial.print(".");
      esp_task_wdt_reset();
    }
    if (WiFi.status() == WL_CONNECTED) break;
    retry++;
    Serial.printf("\n[WiFi] Retry %d/%d...\n", retry, Core::WIFI_STA_MAX_RETRIES);
    WiFi.disconnect(true, true);
    delay(1000);
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    _mode = WifiMode::STA;
    Serial.printf("[WiFi] STA connected! IP: %s, RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Services::Log.append(Core::LogType::Restart,
      "STA connected: " + ssid + " IP:" + WiFi.localIP().toString(), 0);
    return true;
  }

  Serial.println("[WiFi] STA failed — all retries exhausted");
  Services::Log.append(Core::LogType::Error, "STA connect failed", 0);
  WiFi.disconnect(true, true);
  delay(500);
  return false;
}

void WifiManager::_runConfigPortal() {
  Serial.println("[WiFi] Starting Config Portal...");
  _mode = WifiMode::AP_CONFIG;

  // audit-fixes (auditor #2 P0): Config Portal was open AP (password = "").
  //   Anyone within WiFi range during the first-boot window could connect and
  //   hijack the device's WiFi creds. Now uses the same random AP password
  //   generated in generateApPassword(). The owner reads the password from
  //   Serial (dev) or provisioning sheet (prod) — same pattern as fallback AP.
  WiFi.mode(WIFI_AP);
  // _apPassword is the random CSPRNG-generated password (set in begin()).
  // WIFI_CONFIG_PORTAL_PASSWORD is intentionally ignored — it was "" by default.
  WiFi.softAP(Core::WIFI_CONFIG_PORTAL_SSID, _apPassword, Core::WIFI_CHANNEL,
              Core::WIFI_HIDDEN, Core::WIFI_MAX_CLIENTS);

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[WiFi] Config Portal AP: %s\n", Core::WIFI_CONFIG_PORTAL_SSID);
  Serial.printf("[WiFi] Portal IP: %s\n", apIP.toString().c_str());
  Serial.printf("[WiFi] Open http://%s in browser to configure\n", apIP.toString().c_str());
#ifdef PRODUCTION_BUILD
  Serial.println("[WiFi] AP Password: [REDACTED in PRODUCTION_BUILD — see provisioning sheet]");
#else
  Serial.printf("[WiFi] AP Password: %s\n", _apPassword);
#endif

  WebServer configServer(80);

  configServer.on("/", [this, &configServer]() {
    String html = FPSTR(CONFIG_PORTAL_HTML);
    html.replace("__MAC__", getMacAddress());
    // audit-fixes: MQTT password + PIN no longer exposed in config portal HTML.
    //   The owner reads them from Serial (dev) or provisioning sheet (prod).
    configServer.send(200, "text/html", html);
  });

  configServer.on("/save", HTTP_POST, [this, &configServer]() {
    String ssid = configServer.arg("ssid");
    String pass = configServer.arg("pass");
    if (ssid.length() == 0 || ssid.length() > 32) {
      configServer.send(400, "text/plain", "Invalid SSID");
      return;
    }
    _saveCredentials(ssid, pass);
    String html = "<html><body style='font-family:sans-serif;background:#0F172A;color:#10B981;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0'><div style='text-align:center'><h1>Saved!</h1><p>Rebooting... Connect to your WiFi and access PWA.</p></div></body></html>";
    configServer.send(200, "text/html", html);
    delay(2000);
    ESP.restart();
  });

  configServer.onNotFound([&configServer]() {
    configServer.sendHeader("Location", "http://"+WiFi.softAPIP().toString()+"/");
    configServer.send(302, "text/plain", "");
  });

  configServer.begin();

  Serial.println("[WiFi] Config Portal running. Waiting for user input...");
  Serial.println(F("========================================"));
#ifdef PRODUCTION_BUILD
  // audit-fixes: do NOT print credentials to Serial in production.
  Serial.println(F("MQTT Password: [REDACTED in PRODUCTION_BUILD]"));
  Serial.println(F("Device PIN:    [REDACTED in PRODUCTION_BUILD]"));
#else
  Serial.printf("MQTT Password: %s\n", _mqttPassword);
  Serial.printf("Device PIN: %s\n", _devicePin);
#endif
  Serial.println(F("========================================"));

  // Run config portal until device restarts (user saves creds)
  while (true) {
    configServer.handleClient();
    esp_task_wdt_reset();
    vTaskDelay(1);
  }
}

void WifiManager::openConfigPortal() {
  _runConfigPortal();
}

bool WifiManager::_startApFallback() {
  Serial.println("[WiFi] Starting fallback AP mode...");
  _mode = WifiMode::AP_FALLBACK;
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower((wifi_power_t)Core::WIFI_TX_POWER_DBM);

  IPAddress localIP(Core::AP_IP[0], Core::AP_IP[1], Core::AP_IP[2], Core::AP_IP[3]);
  IPAddress gateway(Core::AP_IP[0], Core::AP_IP[1], Core::AP_IP[2], Core::AP_IP[3]);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIP, gateway, subnet);

  if (!WiFi.softAP(Core::AP_SSID, _apPassword, Core::WIFI_CHANNEL,
                   Core::WIFI_HIDDEN, Core::WIFI_MAX_CLIENTS)) {
    Services::Log.append(Core::LogType::Error, "Fallback AP failed", 0);
    return false;
  }

  Serial.printf("[WiFi] Fallback AP: SSID=%s, IP=%s\n",
                Core::AP_SSID, WiFi.softAPIP().toString().c_str());
#ifdef PRODUCTION_BUILD
  // audit-fixes: do NOT print AP password to Serial in production.
  Serial.println("[WiFi] AP Password: [REDACTED in PRODUCTION_BUILD]");
#else
  Serial.printf("[WiFi] AP Password: %s\n", _apPassword);
#endif
  Serial.println("[WiFi] Connect to AP, open http://192.168.4.1 to reconfigure");
  Services::Log.append(Core::LogType::Error,
    "AP fallback — WiFi creds may be wrong", 0);
  return true;
}

bool WifiManager::begin() {
  generateApPassword();
  _loadCredentials();  // Load WiFi creds + generate MQTT password + PIN

  WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
    onEvent(event, info);
  });

  // Try STA first. If no creds or all retries fail → Config Portal.
  if (_tryStaMode()) {
    return true;
  }

  // Check if we have stored creds (failed to connect) vs no creds at all
  Preferences prefs;
  prefs.begin(Core::NVS_NAMESPACE, true);
  String ssid = prefs.getString(Core::NVS_KEY_WIFI_SSID, "");
  prefs.end();

  if (ssid.length() == 0) {
    // No creds at all → Config Portal (first boot)
    _runConfigPortal();
    return false;  // never reached (Config Portal loops forever)
  }

  // Creds exist but failed → fallback AP (user can reconfigure via web)
  // After 60s in fallback AP, auto-open Config Portal
  if (_startApFallback()) {
    return true;
  }

  // Last resort: Config Portal
  _runConfigPortal();
  return false;
}

void WifiManager::onEvent(arduino_event_id_t event, arduino_event_info_t info) {
  (void)info;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Services::Log.append(Core::LogType::Login, "WiFi STA associated", 0);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Services::Log.append(Core::LogType::Login,
        "STA got IP: " + WiFi.localIP().toString(), 0);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Services::Log.append(Core::LogType::Logout, "WiFi STA disconnected", 0);
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Services::Log.append(Core::LogType::Login, "AP client connected", 0);
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Services::Log.append(Core::LogType::Logout, "AP client disconnected", 0);
      break;
    default: break;
  }
}

String WifiManager::getApPassword() const {
  return String(_apPassword);
}

IPAddress WifiManager::getLocalIp() const {
  if (_mode == WifiMode::STA) return WiFi.localIP();
  return WiFi.softAPIP();
}

uint8_t WifiManager::getClientCount() const {
  if (_mode == WifiMode::AP_CONFIG || _mode == WifiMode::AP_FALLBACK) {
    return WiFi.softAPgetStationNum();
  }
  return 0;
}

int WifiManager::getRssi() const {
  if (_mode == WifiMode::STA) return WiFi.RSSI();
  return -55;
}

bool WifiManager::isConnected() const {
  if (_mode == WifiMode::STA) return WiFi.status() == WL_CONNECTED;
  if (_mode == WifiMode::AP_CONFIG || _mode == WifiMode::AP_FALLBACK) return true;
  return false;
}

} // namespace TimerNet
