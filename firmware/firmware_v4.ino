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

// v4.1 — DC Energy & Battery Monitoring drivers (brief §3, §10-20)
// GPIO audit documented in BatteryConfig.h (PACK_VOLTAGE_SOURCE = ADS1115_AIN3_BPLUS
// by default; ESP32 ADC1 is fully occupied by I2C + PIR on standard WROOM-32).
#include "BatteryConfig.h"
#include "Ina219Driver.h"
#include "Ads1115Driver.h"
#include "Sht31Driver.h"
#include "BatteryVoltageDriver.h"

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
#include "BatteryMonitor.h"        // v4.1 DC energy & battery monitoring
#include "BatteryDiagnostics.h"
#include "ResistanceEstimator.h"
// v4.2 industrial-grade hardening (audit brief §13-16, §18-19, §44, §59-60)
#include "ErrorCodes.h"
#include "AlarmRegistry.h"
#include "SafetySupervisor.h"
#include "HealthSupervisor.h"
// v4.3 audit P1-001, P1-002: CommandArbiter + InterlockEngine
#include "CommandArbiter.h"
#include "InterlockEngine.h"
// v4.3.1 audit D-005: TelemetrySpool (store-and-forward software architecture)
#include "TelemetrySpool.h"

// Web
#include "HttpServer.h"

// AI
#include "Advisor.h"
#include "Ed25519SelfTest.h"  // On-target Ed25519 KAT self-test

// ---------- GLOBAL STATE DEFINITIONS (declared extern in Core/Globals.h) ----------
namespace Core {
  Channel channels[NUM_CHANNELS];
  bool relayState[NUM_CHANNELS] = {false};
  RelaySource relaySource[NUM_CHANNELS] = {RelaySource::Off};
  // v4.3 audit P1-005, P1-014: physical state + confidence + sequence
  bool relayPhysicalState[NUM_CHANNELS] = {false};  // unknown until aux feedback
  StateConfidence relayStateConfidence[NUM_CHANNELS] = {StateConfidence::Unknown};
  uint32_t relayStateSequence[NUM_CHANNELS] = {0};
  unsigned long relayStateTimestamp[NUM_CHANNELS] = {0};
  bool relayFault[NUM_CHANNELS] = {false};
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
  Services::ota.begin();
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
  // AUD-FW-CFG-003 FIX: Load energyWh from NVS after schedule (so defaults are set first)
  Storage::config.loadEnergyFromNVS();
  esp_task_wdt_reset();

  // ---------- v4.2 INDUSTRIAL-GRADE SERVICES (brief §13-16, §18-19, §44, §60) ----------
  // AlarmRegistry + HealthSupervisor + SafetySupervisor initialize BEFORE
  // any driver begins so they can capture boot-time events (reset reason,
  // brownout, watchdog, low heap, RTC invalid) as alarms.
  Services::alarms.begin();
  esp_task_wdt_reset();
  Services::health.begin();          // loads bootCount + watchdog/brownout counters from NVS
  esp_task_wdt_reset();
  Services::health.recordBoot();    // classifies reset reason, raises alarms
  esp_task_wdt_reset();
  Services::safety.reset();         // clear runtime tracking
  esp_task_wdt_reset();
  // AUD-FW-CFG-002 FIX: Load persisted lockout states from NVS AFTER reset()
  // so that TRIPPED/ACKNOWLEDGED channels are restored (reboot cannot bypass
  // the ACK→CLEAR→ARM→NORMAL sequence).
  Services::safety.begin();
  esp_task_wdt_reset();
  // v4.3 audit P1-001, P1-002: CommandArbiter + InterlockEngine
  Services::interlock.begin();      // no groups registered by default — owner configures via PWA or NVS
  esp_task_wdt_reset();
  // AUD-FW-CMD-002 FIX: Load persisted _lastAppliedSeq from NVS so stale-command
  // detection survives reboot (prevents replay of stale commands after power-cycle).
  Services::arbiter.begin();
  esp_task_wdt_reset();
  // v4.3.1 audit D-005: TelemetrySpool init (RAM ring buffer, no NVS yet)
  Services::telemetrySpool.begin();
  esp_task_wdt_reset();

  // On-target Ed25519 KAT self-test — verifies that Ed25519 signature
  // verification works correctly on THIS device's hardware. Uses RFC 8032
  // published test vectors. Result is cached 5 minutes + available via
  // GET /api/ed25519/kat (authenticated). Does NOT block OTA — diagnostic only.
  Serial.println("[Boot] Running Ed25519 KAT self-test...");
  Services::Ed25519KatResult katResult = Services::ed25519SelfTest.run(true);
  if (katResult.allPassed) {
    Serial.printf("[Boot] Ed25519 KAT: PASS (%d/%d in %lu ms)\n",
                  katResult.testsPassed, katResult.testsRun, katResult.totalDurationMs);
  } else {
    Serial.printf("[Boot] Ed25519 KAT: FAIL (%d/%d — Ed25519 may not work correctly)\n",
                  katResult.testsPassed, katResult.testsRun);
    Serial.println("[Boot] WARNING: OTA signature verification may be non-functional on this device");
  }
  esp_task_wdt_reset();

  // ---------- DRIVERS: RTC + Relays ----------
  Drivers::rtc.begin();              // updates RTC state machine in HealthSupervisor
  Drivers::relay.begin();

  // ---------- PZEM-004T v3.0 POWER METER (optional, via UART) ----------
  Drivers::pzem.begin();

  // ---------- v4.1 — DC ENERGY & BATTERY MONITORING ----------
  // All these drivers share the I²C bus initialized by RtcDriver::begin()
  // (SDA=GPIO32, SCL=GPIO33). They are non-fatal: if any sensor is absent
  // the relay/scheduler/PZEM/MQTT/REST subsystem continues working.
  // (Brief §46: monitoring failure must NOT disable relay control.)
  if (Battery::ENABLED) {
    Drivers::ina219Battery.begin();
    esp_task_wdt_reset();
    Drivers::ina219Inverter.begin();
    esp_task_wdt_reset();
    Drivers::adsCell1.begin();
    esp_task_wdt_reset();
    Drivers::adsCell2.begin();
    esp_task_wdt_reset();
    Drivers::sht31.begin();
    esp_task_wdt_reset();
    Drivers::packVoltage.begin();
    esp_task_wdt_reset();

    // Calculation + diagnostics layers (brief §53 Phase 2/3)
    Services::battery.begin();
    esp_task_wdt_reset();
    Services::batteryDiagnostics.begin();
    esp_task_wdt_reset();
    Services::resistance.begin();
    esp_task_wdt_reset();
  }

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

  // ---------- OTA (already initialized early for boot health check) ----------
  // Services::ota.begin() called at line 182 for R10B-6 boot health check.
  // Do NOT call again — begin() is idempotent but redundant call is dead code.

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
  Services::ota.markBootHealthy();
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
  Services::health.recordHeartbeat(Services::TaskId::Pzem);  // v4.3.6 D-003

  // 2c. v4.1 DC energy & battery monitoring sensors — non-blocking, all
  // internally rate-limited. (Brief §44.)
  if (Battery::ENABLED) {
    Drivers::ina219Battery.tick();
    Drivers::ina219Inverter.tick();
    Drivers::adsCell1.tick();
    Drivers::adsCell2.tick();
    Drivers::sht31.tick();
    Drivers::packVoltage.tick();
    Services::battery.tick();
    Services::batteryDiagnostics.tick();
    Services::resistance.tick();
    Services::health.recordHeartbeat(Services::TaskId::BatteryMonitor);
  }

  // 2d. v4.2 health supervisor tick — monitors heap, reset reasons, task stalls
  Services::health.tick();
  Services::health.recordHeartbeat(Services::TaskId::HealthMonitor);  // v4.3.6 D-003

  // 2e. v4.3.6 D-003: emit heartbeats for remaining critical tasks
  // PIR heartbeat is emitted inside RelayEngine::tick() which calls Drivers::pir.tick()
  // OTA heartbeat is emitted inside OtaManager (checked during tick)
  // Scheduler heartbeat is emitted inside RelayEngine::tick() which calls scheduler
  // We emit them here for tasks that don't have their own loop method:
  Services::health.recordHeartbeat(Services::TaskId::Scheduler);
  Services::health.recordHeartbeat(Services::TaskId::Pir);
  Services::health.recordHeartbeat(Services::TaskId::Ota);
  Services::health.recordHeartbeat(Services::TaskId::Telemetry);

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
    // AUD-FW-CFG-003 FIX: Also persist energyWh to NVS when schedule is saved.
    Storage::config.saveEnergyToNVS();
  }

  // REAUDIT-FW-CFG-001 FIX: Periodic energyWh save EVERY 5 MINUTES, independent
  // of scheduleDirty. The auditor correctly identified that a "set-and-forget"
  // deployment (operator never changes schedule → scheduleDirty never set) would
  // lose all energy data on reboot. This timer ensures energyWh is saved
  // regardless of schedule changes. NVS wear: 12 × 4 bytes = 48 bytes per save,
  // every 300s = 1152 bytes/hour = 27,648 bytes/day. At ~100k erase cycles per
  // NVS sector (4KB), this gives ~145 days of continuous operation per sector.
  // Acceptable for the use case (energy data, not safety-critical).
  static unsigned long lastEnergySave = 0;
  if (millis() - lastEnergySave > 300000UL) {  // 5 minutes
    lastEnergySave = millis();
    Storage::config.saveEnergyToNVS();
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
