// =============================================================================
// Timer Digital Relay v4.0 — Cloud-Ready Architecture
// Main firmware entry point (setup + loop)
// -----------------------------------------------------------------------------
// Board: ESP32 DEV MODULE WROOM
// Architecture: Modular (Core/Drivers/Services/Storage/Network/Web/Utils/AI)
//
// CHANGELOG v4.0.0:
//   [MAJOR] Modular refactor — split monolithic 2744-line .ino into:
//     Core/      — config, types, globals
//     Drivers/   — Relay, PIR, RTC
//     Storage/   — FileSystem (LittleFS), ConfigStore (atomic + CRC + backup)
//     Utils/     — CRC, Crypto (SHA-256/PBKDF2/HMAC/JWT), JSON helpers
//     Network/   — WiFi AP manager
//     Services/  — Scheduler, RelayEngine, AuthManager, OtaManager, LogService
//     Web/       — HTTP server + 22 v4.0 route handlers
//     AI/        — Advisory stub (future GAS/Gemini pipeline)
//
//   [MAJOR] v4.0 API contract — endpoints renamed & extended:
//     OLD (v3.1)              → NEW (v4.0)
//     /api/manual?ch=X&state=Y → /api/relay (POST JSON)
//     /api/save_channel?ch=X   → /api/schedule (POST JSON, per-schedule)
//     /api/set_time?datetime=  → /api/time (POST JSON)
//     /api/audit_log (text)    → /api/log (JSON, filterable)
//     /api/config (basic auth) → /api/login (JWT) + /api/session + /api/config/*
//     + /api/pir, /api/pir/test, /api/ota/check, /api/config/device,
//       /api/config/password, /api/config/export, /api/config/import,
//       /api/factory_reset/prepare, /api/factory_reset/confirm
//
//   [MAJOR] Authentication: Basic Auth → JWT (HS256) + CSRF cookie
//     - POST /api/login returns JWT (httpOnly cookie) + CSRF token (readable cookie)
//     - Mutations require X-CSRF-Token header
//     - Stateless: server verifies JWT signature + expiry, no session store
//
//   [MINOR] Per-schedule enable flag (was: all-or-nothing per channel)
//   [MINOR] Activity log as JSON-lines (was: plain text only)
//   [MINOR] Device name + timezone moved to NVS Preferences
//   [MINOR] CORS headers for PWA cross-origin (Vercel → ESP32 via Cloudflare Tunnel)
//
// PRIORITY ORDER (unchanged from v3.1):
//   1. Manual mode (modeAuto=false) → manualState wins
//   2. PIR override (modeAuto=true, pirEnabled, PIR active) → ON
//   3. Schedule (modeAuto=true, schedule active) → ON
//   4. Default → OFF
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <Preferences.h>

// Core
#include "Config.h"
#include "Types.h"
#include "Globals.h"

// Utils
#include "Crypto.h"
#include "Crc.h"
#include "Json.h"

// Storage
#include "FileSystem.h"
#include "ConfigStore.h"

// Drivers
#include "RelayDriver.h"
#include "PirDriver.h"
#include "RtcDriver.h"
#include "PzemDriver.h"

// Network
#include "WifiManager.h"

// Services
#include "LogService.h"
#include "Scheduler.h"
#include "RelayEngine.h"
#include "AuthManager.h"
#include "OtaManager.h"
#include "MqttClient.h"
#include "TransactionJournal.h"  // R10G-1: NVS transaction journal

// Web
#include "HttpServer.h"

// AI
#include "Advisor.h"

// ---------- GLOBAL STATE DEFINITIONS (declared extern in Core/Globals.h) ----------
namespace Core {
  Channel channels[NUM_CHANNELS];
  bool relayState[NUM_CHANNELS] = {false};
  RelaySource relaySource[NUM_CHANNELS] = {RelaySource::Off};
  PirState pirState[NUM_PIR];
  UserConfig userConfig = {"admin", "", {0}, PBKDF2_ITERATIONS};
  SystemMetrics metrics = {0, 0, 0, {0}, true};
  bool timeValid = false;
  bool scheduleDirty = false;
  bool firstDirtySet = false;
  unsigned long lastSaveTime = 0;
  unsigned long firstDirtyTime = 0;
  unsigned long pirStartupTime = 0;
  char csrfToken[CSRF_TOKEN_LEN + 1] = {0};
  unsigned long csrfTokenTime = 0;
  char apPassword[33] = {0};
  char deviceName[33] = "Timer12-ESP32";
  char timezone[40] = "Asia/Jakarta";
  char jwtSecret[65] = {0};
  AuthAttempt authAttempts[MAX_TRACKED_IPS];
  char factoryResetToken[33] = {0};
  unsigned long factoryResetTokenTime = 0;
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n========================================"));
  Serial.printf("Timer 12 Relay v%s\n", Core::FIRMWARE_VERSION);
  Serial.printf("Build: %s\n", Core::BUILD_DATE);
  Serial.println(F("Cloud-Ready Architecture (modular)"));
  Serial.println(F("========================================"));

  // ---------- WATCHDOG INIT ----------
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 10000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&twdt_config);
#else
  esp_task_wdt_init(10, true);
#endif
  esp_task_wdt_add(NULL);

  // ---------- INIT DRIVERS (PIR + Relays before storage to avoid glitches) ----------
  Drivers::pir.begin();
  esp_task_wdt_reset();

  // ---------- FILESYSTEM ----------
  if (!Storage::fs.begin()) {
    Serial.println(F("FATAL: LittleFS failed. Restart."));
    delay(1000); ESP.restart();
  }
  Storage::fs.cleanupTempFiles();
  esp_task_wdt_reset();

  // R10B-6: OTA boot health check — must run EARLY (before any subsystem that
  // could crash). Increments boot_attempts in NVS. If attempts > MAX (3),
  // triggers rollback to previous firmware partition.
  Services::otaManager.begin();
  esp_task_wdt_reset();

  // ---------- SERVICES (LOG) ----------
  Services::Log.begin();

  // ---------- STORAGE: load configs ----------
  Storage::config.loadDeviceConfig();
  esp_task_wdt_reset();
  Storage::config.loadUserConfig();
  esp_task_wdt_reset();
  Storage::config.loadSchedule();
  esp_task_wdt_reset();

  // ---------- DRIVERS: RTC + Relays ----------
  Drivers::rtc.begin();
  Drivers::relay.begin();

  // ---------- PZEM-004T v3.0 POWER METER (optional, via UART) ----------
  Drivers::pzem.begin();

  // ---------- INITIAL RELAY COMPUTATION ----------
  Services::relayEngine.forceRefresh();

  // ---------- NETWORK: WiFi STA (join MiFi) + AP fallback ----------
  Serial.println(F("========================================"));
  Serial.println(F("WiFi: starting (STA primary, AP fallback)"));
  Serial.println(F("========================================"));
  TimerNet::wifi.begin();
  Serial.print(F("IP: "));
  Serial.println(TimerNet::wifi.getLocalIp());
  Serial.printf("Mode: %s\n", TimerNet::wifi.getMode() == TimerNet::WifiMode::STA ? "STA" : "AP");
  Serial.printf("MAC: %s\n", TimerNet::wifi.getMacAddress().c_str());

  // ---------- AUTH ----------
  Services::auth.begin();

  // ---------- OTA ----------
  Services::ota.begin();

  // ---------- MQTT (remote internet access via CGNAT) ----------
  Services::mqtt.begin();

  // ---------- AI ADVISOR (stub) ----------
  AI::advisor.begin("");  // GAS URL not configured yet

  // ---------- WEB SERVER ----------
  Web::server.begin();

  // ---------- BOOT COMPLETE ----------
  Core::metrics.bootTime = millis() / 1000;
  Services::Log.append(Core::LogType::Restart,
    String("System boot complete v") + Core::FIRMWARE_VERSION, 0);
  Serial.println(F("Boot complete. Ready."));

  // R10B-6: Mark firmware as healthy (cancels rollback).
  // All critical subsystems (LittleFS, WiFi, RTC, Relay, MQTT, WebServer)
  // have initialized successfully. If this is first boot after OTA, the
  // firmware is now marked VALID — rollback will not trigger on next boot.
  Services::otaManager.markBootHealthy();
  esp_task_wdt_reset();
}

// ---------- LOOP ----------
void loop() {
  // 1. Handle HTTP requests (LAN access)
  Web::server.handleClient();
  esp_task_wdt_reset();

  // 2. MQTT loop (remote internet access)
  Services::mqtt.loop();
  esp_task_wdt_reset();

  // 2b. PZEM power meter read (every 1s)
  Drivers::pzem.tick();

  // 3. Recompute relay states every 1s (catch up if loop was slow)
  static unsigned long lastTick = 0;
  int catchUp = 0;
  while (millis() - lastTick >= Core::RELAY_TICK_MS && catchUp < 5) {
    lastTick += Core::RELAY_TICK_MS;
    Services::relayEngine.tick();
    catchUp++;
  }
  if (catchUp >= 5) lastTick = millis();

  // 4. Auto-save schedule if dirty (debounced)
  if (Core::scheduleDirty &&
      (millis() - Core::lastSaveTime > Core::SAVE_DELAY_MS ||
       (Core::firstDirtySet && millis() - Core::firstDirtyTime > Core::MAX_SAVE_DELAY_MS))) {
    Storage::config.saveSchedule();
  }

  // 5. Daily counter reset (PIR triggers, error count)
  static unsigned long lastDayCheck = 0;
  if (millis() - lastDayCheck > 60000) {  // check every minute
    lastDayCheck = millis();
    if (Core::timeValid) {
      int y, m, d, h, mi, s, wd;
      Drivers::rtc.getDateTime(y, m, d, h, mi, s, wd);
      uint32_t today = (uint32_t)y * 10000 + m * 100 + d;
      if (today != Core::metrics.lastDailyResetDay) {
        Drivers::pir.resetDailyCounters();
        Core::metrics.lastDailyResetDay = today;
      }
    }
  }

  // 6. AI advisor tick (future GAS sync — currently no-op)
  AI::advisor.tick();

  vTaskDelay(1);
}
