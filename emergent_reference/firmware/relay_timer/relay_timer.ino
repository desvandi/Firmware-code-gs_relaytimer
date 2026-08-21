/**
 * ============================================================================
 *  RELAY TIMER — ESP32 Generic Firmware (Flash-Once / Captive Portal)
 *  Repo: Firmware-code-gs_relaytimer
 * ----------------------------------------------------------------------------
 *  Paradigma: satu binary generik untuk semua user. Semua konfigurasi diisi
 *  runtime via Captive Portal dan disimpan di LittleFS (/config.json).
 *
 *  Dependencies (Library Manager):
 *    - ESPAsyncWebServer  (me-no-dev)
 *    - AsyncTCP           (me-no-dev)
 *    - ArduinoJson        (bblanchon)
 *    - LittleFS           (bawaan core ESP32)
 *
 *  State machine:
 *    BOOT -> init LittleFS -> cek tombol BOOT(GPIO0) ditahan >5s ?
 *        YA / config invalid -> AP MODE (Captive Portal @192.168.4.1)
 *        TIDAK                -> STA MODE (Normal Running)
 *
 *  Factory reset: tahan GPIO0 selama 10 detik saat booting -> hapus config.
 * ============================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <time.h>

/* ------------------------------- Konstanta ------------------------------- */
#define CONFIG_FILE        "/config.json"
#define BOOT_BUTTON_PIN    0            // GPIO0 (tombol BOOT)
#define LED_PIN            2            // LED onboard
#define AP_ENTER_HOLD_MS   5000         // tahan >5s -> paksa AP mode
#define FACTORY_HOLD_MS    10000        // tahan 10s -> factory reset
#define WDT_TIMEOUT_S      15           // Task Watchdog Timer
#define RECONNECT_WINDOW_MS 30000       // coba reconnect 30 detik
#define FALLBACK_AP_MS     600000       // AP cadangan 10 menit
#define BG_RETRY_MS        60000        // retry WiFi utama tiap 60 detik
#define HEARTBEAT_MS       15000        // heartbeat default (dioverride config)

const char* AP_PASSWORD = "relaysetup"; // AP password sederhana
const char* NTP_SERVER  = "pool.ntp.org";

/* ------------------------------- Global ---------------------------------- */
DNSServer dnsServer;
AsyncWebServer server(80);

struct Config {
  String wifi_ssid;
  String wifi_pass;
  String gas_url;
  String auth_token;
  String device_key;
  int    heartbeat_interval_sec = 15;
  int    relay_gpio[8];
  int    relay_count = 0;
};
Config cfg;

enum Mode { MODE_AP, MODE_STA };
Mode currentMode = MODE_STA;

unsigned long lastHeartbeat = 0;
unsigned long fallbackApStart = 0;
unsigned long lastBgRetry = 0;
bool fallbackApActive = false;

/* ======================================================================== */
/*  LittleFS CONFIG PERSISTENCE                                             */
/* ======================================================================== */

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) return false;
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return false;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[CFG] Parse error: %s\n", err.c_str());
    return false;
  }

  cfg.wifi_ssid   = doc["wifi_ssid"]   | "";
  cfg.wifi_pass   = doc["wifi_pass"]   | "";
  cfg.gas_url     = doc["gas_url"]     | "";
  cfg.auth_token  = doc["auth_token"]  | "";
  cfg.device_key  = doc["device_key"]  | "";
  cfg.heartbeat_interval_sec = doc["heartbeat_interval_sec"] | 15;

  cfg.relay_count = 0;
  JsonArray arr = doc["relay_gpio_map"].as<JsonArray>();
  for (JsonVariant v : arr) {
    if (cfg.relay_count < 8) cfg.relay_gpio[cfg.relay_count++] = v.as<int>();
  }
  if (cfg.relay_count == 0) { // default fallback
    int def[] = {26, 27, 14, 12};
    for (int i = 0; i < 4; i++) cfg.relay_gpio[i] = def[i];
    cfg.relay_count = 4;
  }

  // Valid jika kredensial inti terisi
  return cfg.wifi_ssid.length() > 0 && cfg.gas_url.length() > 0 &&
         cfg.auth_token.length() >= 16 && cfg.device_key.length() > 0;
}

bool saveConfig(JsonDocument& doc) {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return false;
  serializeJsonPretty(doc, f);
  f.close();
  return true;
}

// Factory reset: hapus config (HANYA dipicu tombol fisik).
void factoryReset() {
  Serial.println("[RESET] Menghapus /config.json ...");
  if (LittleFS.exists(CONFIG_FILE)) LittleFS.remove(CONFIG_FILE);
  // Indikator: LED berkedip cepat 5x
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }
  ESP.restart();
}

/* ======================================================================== */
/*  CAPTIVE PORTAL (AP MODE)                                                */
/* ======================================================================== */

String apSsid() {
  uint64_t chip = ESP.getEfuseMac();
  char suffix[5];
  sprintf(suffix, "%04X", (uint16_t)(chip & 0xFFFF));
  return String("RelayTimer-Setup-") + suffix;
}

const char SETUP_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Relay Timer Setup</title>
<style>
 body{font-family:system-ui;background:#09090b;color:#fff;margin:0;padding:24px;max-width:520px;margin:auto}
 h1{font-size:20px} label{display:block;margin:14px 0 4px;font-size:13px;color:#a1a1aa}
 input,select{width:100%;padding:12px;border-radius:8px;border:1px solid #27272a;background:#18181b;color:#fff;box-sizing:border-box}
 button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:8px;background:#007AFF;color:#fff;font-weight:600;font-size:15px}
 small{color:#71717a}
</style></head><body>
<h1>⚡ Relay Timer — Setup</h1>
<form method="POST" action="/save">
 <label>WiFi SSID</label>
 <select name="wifi_ssid" id="ssid"></select>
 <label>WiFi Password</label>
 <input name="wifi_pass" type="password">
 <label>GAS WebApp URL</label>
 <input name="gas_url" type="url" placeholder="https://script.google.com/macros/s/.../exec" required>
 <label>Auth Token</label>
 <input name="auth_token" required minlength="16">
 <label>Device Key</label>
 <input name="device_key" value="RELAY_CTRL_01" required>
 <label>Relay GPIO (pisahkan koma)</label>
 <input name="relay_gpio_map" value="26,27,14,12">
 <label>Heartbeat Interval (detik)</label>
 <input name="heartbeat_interval_sec" type="number" value="15">
 <button type="submit">Simpan &amp; Reboot</button>
 <p><small>Perangkat akan reboot dan konek ke WiFi rumah Anda.</small></p>
</form>
<script>
 fetch('/scan').then(r=>r.json()).then(list=>{
   var s=document.getElementById('ssid');
   list.forEach(n=>{var o=document.createElement('option');o.value=n;o.text=n;s.add(o);});
 });
</script></body></html>
)HTML";

void handleScan(AsyncWebServerRequest* req) {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "\"" + WiFi.SSID(i) + "\"";
  }
  json += "]";
  req->send(200, "application/json", json);
}

void handleSave(AsyncWebServerRequest* req) {
  StaticJsonDocument<1024> doc;
  doc["wifi_ssid"]  = req->arg("wifi_ssid");
  doc["wifi_pass"]  = req->arg("wifi_pass");
  doc["gas_url"]    = req->arg("gas_url");
  doc["auth_token"] = req->arg("auth_token");
  doc["device_key"] = req->arg("device_key");
  doc["heartbeat_interval_sec"] = req->arg("heartbeat_interval_sec").toInt();

  JsonArray arr = doc.createNestedArray("relay_gpio_map");
  String map = req->arg("relay_gpio_map");
  int start = 0;
  while (start < map.length()) {
    int comma = map.indexOf(',', start);
    if (comma == -1) comma = map.length();
    arr.add(map.substring(start, comma).toInt());
    start = comma + 1;
  }

  if (saveConfig(doc)) {
    req->send(200, "text/html",
      "<html><body style='font-family:system-ui;background:#09090b;color:#fff;padding:40px'>"
      "<h2>✅ Tersimpan!</h2><p>Perangkat reboot dalam 3 detik...</p></body></html>");
    delay(3000);
    ESP.restart();
  } else {
    req->send(500, "text/plain", "Gagal menyimpan config.");
  }
}

void startAPMode() {
  currentMode = MODE_AP;
  Serial.println("[MODE] AP / Captive Portal");
  WiFi.mode(WIFI_AP);
  String ssid = apSsid();
  WiFi.softAP(ssid.c_str(), AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP(); // 192.168.4.1
  Serial.printf("[AP] SSID: %s  IP: %s\n", ssid.c_str(), ip.toString().c_str());

  dnsServer.start(53, "*", ip); // DNS redirect -> captive portal

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html", SETUP_HTML);
  });
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  // Captive portal: semua request tak dikenal -> form setup
  server.onNotFound([](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html", SETUP_HTML);
  });
  server.begin();
}

/* ======================================================================== */
/*  STA MODE (NORMAL RUNNING)                                               */
/* ======================================================================== */

bool connectWiFi(unsigned long windowMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
  Serial.printf("[STA] Konek ke %s ...\n", cfg.wifi_ssid.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < windowMs) {
    delay(300);
    Serial.print(".");
    esp_task_wdt_reset();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH);
    Serial.printf("\n[STA] Terhubung. IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\n[STA] Gagal terhubung.");
  return false;
}

void syncTime() {
  configTime(0, 0, NTP_SERVER);
  struct tm t;
  int tries = 0;
  while (!getLocalTime(&t) && tries++ < 10) { delay(300); esp_task_wdt_reset(); }
  Serial.println("[NTP] Waktu tersinkron.");
}

void applyRelayStates() {
  for (int i = 0; i < cfg.relay_count; i++) {
    pinMode(cfg.relay_gpio[i], OUTPUT);
    digitalWrite(cfg.relay_gpio[i], LOW); // default OFF on boot
  }
}

// Kirim heartbeat + poll perintah relay. WAJIB http.end() untuk cegah leak.
void pollRelayCommands(WiFiClientSecure& client); // forward decl (ref param)
void heartbeatAndPoll() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // GAS pakai HTTPS Google; skip cert pinning
  HTTPClient http;
  http.setTimeout(8000);

  if (!http.begin(client, cfg.gas_url)) {
    Serial.println("[HTTP] begin gagal");
    return;
  }
  http.addHeader("Content-Type", "text/plain");

  StaticJsonDocument<256> body;
  body["action"]    = "HEARTBEAT";
  body["token"]     = cfg.auth_token;
  body["device"]    = cfg.device_key;
  body["free_heap"] = ESP.getFreeHeap();
  body["uptime"]    = millis() / 1000;
  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  Serial.printf("[HTTP] Heartbeat -> %d, freeHeap=%u\n", code, ESP.getFreeHeap());

  if (code == 200) {
    // Poll status relay untuk dieksekusi
    pollRelayCommands(client);
  } else if (code >= 500) {
    // PRODUCTION GRADE: JANGAN format config saat error 5xx.
    Serial.println("[HTTP] Server 5xx — config dipertahankan.");
  }
  http.end(); // WAJIB
}

void pollRelayCommands(WiFiClientSecure& client) {
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, cfg.gas_url)) return;
  http.addHeader("Content-Type", "text/plain");

  StaticJsonDocument<192> body;
  body["action"] = "POLL";
  body["token"]  = cfg.auth_token;
  body["device"] = cfg.device_key;
  String payload; serializeJson(body, payload);

  int code = http.POST(payload);
  if (code == 200) {
    String resp = http.getString();
    StaticJsonDocument<2048> doc;
    if (!deserializeJson(doc, resp)) {
      JsonArray relays = doc["data"]["relays"].as<JsonArray>();
      for (JsonObject r : relays) {
        int idx = r["relay"];
        const char* state = r["state"] | "OFF";
        if (idx >= 0 && idx < cfg.relay_count) {
          digitalWrite(cfg.relay_gpio[idx], (strcmp(state, "ON") == 0) ? HIGH : LOW);
        }
      }
    }
  }
  http.end(); // WAJIB
}

/* ======================================================================== */
/*  BOOT BUTTON HANDLING                                                    */
/* ======================================================================== */

// Return: 0=normal, 1=enter AP, 2=factory reset
int checkBootButton() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_BUTTON_PIN) == HIGH) return 0; // tidak ditekan

  unsigned long start = millis();
  Serial.println("[BTN] Tombol BOOT ditekan...");
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    unsigned long held = millis() - start;
    esp_task_wdt_reset();
    // kedip lebih cepat saat mendekati factory reset
    digitalWrite(LED_PIN, (held / 150) % 2);
    if (held >= FACTORY_HOLD_MS) return 2;
    delay(20);
  }
  unsigned long held = millis() - start;
  if (held >= AP_ENTER_HOLD_MS) return 1;
  return 0;
}

/* ======================================================================== */
/*  SETUP & LOOP                                                            */
/* ======================================================================== */

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(LED_PIN, OUTPUT);

  // Task Watchdog Timer (ESP32 Arduino core 3.x API)
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS gagal mount!");
  }

  int btn = checkBootButton();
  if (btn == 2) {
    factoryReset(); // tidak kembali (reboot)
  }

  bool haveConfig = loadConfig();

  if (btn == 1 || !haveConfig) {
    startAPMode();
    return;
  }

  // STA MODE
  esp_task_wdt_reset();
  if (connectWiFi(RECONNECT_WINDOW_MS)) {
    syncTime();
    applyRelayStates();
    currentMode = MODE_STA;
    lastHeartbeat = 0;
  } else {
    // Gagal konek: nyalakan AP cadangan 10 menit (JANGAN hapus config)
    Serial.println("[STA] Aktifkan fallback AP 10 menit.");
    startAPMode();
    fallbackApActive = true;
    fallbackApStart = millis();
    lastBgRetry = millis();
  }
}

void loop() {
  esp_task_wdt_reset();

  if (currentMode == MODE_AP) {
    dnsServer.processNextRequest();

    if (fallbackApActive) {
      // Coba reconnect WiFi utama tiap 60 detik di background
      if (millis() - lastBgRetry > BG_RETRY_MS) {
        lastBgRetry = millis();
        Serial.println("[FALLBACK] Retry WiFi utama...");
        if (connectWiFi(10000)) {
          Serial.println("[FALLBACK] Pulih! Reboot ke STA.");
          ESP.restart();
        }
      }
      // Fallback AP hanya 10 menit lalu reboot untuk coba lagi bersih
      if (millis() - fallbackApStart > FALLBACK_AP_MS) {
        Serial.println("[FALLBACK] Timeout 10 menit, reboot.");
        ESP.restart();
      }
    }
    return;
  }

  // STA MODE
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[STA] Koneksi putus, reconnect...");
    if (!connectWiFi(RECONNECT_WINDOW_MS)) {
      startAPMode();
      fallbackApActive = true;
      fallbackApStart = millis();
      lastBgRetry = millis();
      return;
    }
  }

  int interval = cfg.heartbeat_interval_sec > 0 ? cfg.heartbeat_interval_sec : 15;
  if (millis() - lastHeartbeat > (unsigned long)interval * 1000) {
    lastHeartbeat = millis();
    heartbeatAndPoll();
  }
}
