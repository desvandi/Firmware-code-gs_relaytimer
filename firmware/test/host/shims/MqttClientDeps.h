// =============================================================================
// MqttClientDeps.h — Single comprehensive host-side shim header for compiling
//                   firmware/MqttClient.cpp (+ TransactionJournal.cpp +
//                   JournalRecord.cpp) on the host under g++.
// =============================================================================
// FORCE-INCLUDED via `g++ -include shims/MqttClientDeps.h` in EVERY translation
// unit BEFORE its own #includes. This ensures all stubbed Core:: constants,
// ESP32 system stubs, and framework stub classes are visible when Types.h /
// Globals.h / MqttClient.h are parsed inside MqttClient.cpp's TU.
//
// WHAT THIS SHIM PROVIDES (in addition to the existing foundation shims):
//   - All Core:: constants (replacement for firmware/Config.h)
//   - All Core:: struct types (replacement for firmware/Types.h):
//       Schedule, Channel, UserConfig, LogType, ActivityLogEntry, RelaySource,
//       AuthAttempt, PirState, SystemMetrics
//   - All Core:: globals (extern declarations — replaces firmware/Globals.h):
//       channels, relayState, relaySource, pirState, userConfig, metrics,
//       timeValid, scheduleDirty, firstDirtySet, lastSaveTime, firstDirtyTime,
//       pirStartupTime, csrfToken, csrfTokenTime, apPassword, deviceName,
//       timezone, jwtSecret, authAttempts, factoryResetToken,
//       factoryResetTokenTime
//   - All firmware namespace class stubs:
//       Services::LogServiceClass + extern Log
//       Services::RelayEngine     + extern relayEngine
//       Services::Scheduler       + extern scheduler
//       Services::AuthManager     + extern auth
//       Drivers::RtcDriver        + extern rtc
//       Drivers::RelayDriver      + extern relay
//       Drivers::PirDriver        + extern pir
//       Drivers::PzemDriver       + extern pzem
//       Storage::ConfigStore      + extern config
//       Storage::FileSystem       + extern fs
//       TimerNet::WifiManager     + extern wifi
//       Web::HttpServer           + extern server, http
//   - Utils:: functions (replacement for firmware/Crypto.h, Json.h, Crc.h):
//       sha256Hex, bytesToHex, ed25519VerifyHash, parseMinutes, isValidDate,
//       calculateCRC, plus stubs for unused Crypto functions
//   - ESP32 framework stubs:
//       WiFiClient, WiFiClientSecure, PubSubClient, HTTPClient,
//       UpdateClass + Update, RTC_DS3231, WebServer, IPAddress
//   - ESP32 system stubs:
//       ESP (EspClass with restart() → sets flag), delay(), esp_task_wdt_*,
//       arduino_event_id_t / arduino_event_info_t typedefs
//   - mbedtls/sha256.h shim (real SHA-256 via OpenSSL EVP API — not the
//     deprecated SHA256_Init/Update/Final)
//
// WHAT THIS SHIM DOES NOT PROVIDE:
//   - Services::TransactionJournal class + journal global — those come from
//     the real firmware/TransactionJournal.h + TransactionJournal.cpp (compiled
//     alongside MqttClient.cpp). We deliberately do NOT skip
//     TransactionJournal.h's include guard — the real header is processed
//     normally so the class declaration matches the compiled .cpp.
//   - Services::JournalRecord struct + serialize/deserialize functions —
//     from the real firmware/JournalRecord.h + JournalRecord.cpp.
//   - Services::MqttClient class + mqtt global — from the real
//     firmware/MqttClient.h + defined in MqttClient.cpp itself.
//
// INCLUDE GUARD TRICK:
//   When MqttClient.cpp does `#include "LogService.h"` (with quotes), GCC
//   first searches the current file's directory (firmware/) and finds the
//   real firmware/LogService.h. To prevent the real header from being
//   processed (and pulling in transitive ESP32 deps), we DEFINE its include
//   guard BEFORE the #include is reached. This makes the real header a no-op.
//   The alternative definitions provided below in this header take effect.
//
// IMPORTANT:
//   - This file is ONLY compiled for host tests. The ESP32 build uses the
//     real headers from the arduino-esp32 framework.
//   - Production source files (firmware/*.cpp, firmware/*.h) are NOT modified.
//   - TransactionJournal.h, JournalRecord.h, and MqttClient.h are NOT skipped
//     (their include guards are NOT pre-defined here) — the real headers are
//     processed so that class declarations match the compiled .cpp files.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_MQTTCLIENT_DEPS_H
#define HOST_SHIM_MQTTCLIENT_DEPS_H

// ============================================================================
// Part 1: Foundation shims (already existing in shims/)
//   - Arduino.h:     String (full method set), Serial, millis, min, HIGH, LOW
//   - Preferences.h: NVS emulation (putBytes/getBytes/putUChar/getUChar/
//                    putULong/getULong/remove/isKey/begin/end + fail injection)
//   - esp_crc.h:     esp_crc32_le (CRC-32/ISO-HDLC table-driven)
// ============================================================================
#include "Arduino.h"
#include "Preferences.h"
#include "esp_crc.h"

// ============================================================================
// Part 2: Standard library + OpenSSL EVP (for real SHA-256 — avoids the
//         deprecated SHA256_Init/Update/Final API that breaks -Werror on
//         OpenSSL 3.0+).
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <functional>

#include <openssl/evp.h>

// ============================================================================
// Suppress unused-parameter / unused-variable warnings for the rest of the TU.
//
// MqttClient.cpp has several intentional unused parameters/variables (e.g.,
// the `topic` parameter in the publish-callback lambda, `currentMin` in
// publishStatus, `requestId` in _downloadAndVerifyOta). With -Werror these
// would block compilation. The existing test setup (Makefile.mc) uses
// -Wno-unused-parameter etc. — here we achieve the same effect via pragma so
// the user's compile command (which omits those flags) still works.
// ============================================================================
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
// MqttClient.cpp line 1112 (Core::relayState[idx]) triggers a false-positive
// array-bounds warning: the compiler can't trace that `idx` was bounded by
// the channelId validation (1..NUM_CHANNELS → idx 0..11) earlier in the
// function. idx is uint8_t derived from int channelId; the compiler assumes
// the worst case (255). Safe to suppress — production code paths validate
// channelId before this point.
#pragma GCC diagnostic ignored "-Warray-bounds"

// ============================================================================
// Part 3: Skip real firmware headers we provide alternative definitions for.
//
// When MqttClient.cpp does `#include "LogService.h"` (quotes), GCC searches
// firmware/ first (current-file-dir) and finds the real firmware/LogService.h.
// Pre-defining its include guard (TIMER12_SERVICES_LOG_H) makes the file a
// no-op so the alternative class definition below takes effect.
//
// NOT skipped (compiled from .cpp):
//   - TransactionJournal.h (TIMER12_SERVICES_TRANSACTION_JOURNAL_H)
//   - JournalRecord.h      (TIMER12_SERVICES_JOURNAL_RECORD_H)
//   - MqttClient.h          (TIMER12_MQTT_CLIENT_H) — declares the class
//     implemented in MqttClient.cpp
// ============================================================================
#define TIMER12_CORE_CONFIG_H           // skip firmware/Config.h
#define HOST_SHIM_CONFIG_H               // skip existing shims/Config.h (defense)
#define TIMER12_CORE_TYPES_H             // skip firmware/Types.h
#define TIMER12_CORE_GLOBALS_H           // skip firmware/Globals.h
#define TIMER12_SERVICES_LOG_H           // skip firmware/LogService.h
#define TIMER12_SERVICES_RELAY_ENGINE_H // skip firmware/RelayEngine.h
#define TIMER12_SERVICES_SCHEDULER_H     // skip firmware/Scheduler.h
#define TIMER12_DRIVERS_RTC_H            // skip firmware/RtcDriver.h
#define TIMER12_DRIVERS_RELAY_H          // skip firmware/RelayDriver.h
#define TIMER12_DRIVERS_PIR_H            // skip firmware/PirDriver.h
#define TIMER12_PZEM_DRIVER_H            // skip firmware/PzemDriver.h
#define TIMER12_SERVICES_AUTH_H          // skip firmware/AuthManager.h
#define TIMER12_NETWORK_WIFI_H           // skip firmware/WifiManager.h
#define TIMER12_UTILS_CRYPTO_H           // skip firmware/Crypto.h
#define TIMER12_UTILS_JSON_H             // skip firmware/Json.h
#define TIMER12_STORAGE_CONFIG_STORE_H   // skip firmware/ConfigStore.h
#define TIMER12_UTILS_CRC_H              // skip firmware/Crc.h
#define TIMER12_WEB_SERVER_H             // skip firmware/HttpServer.h
#define TIMER12_STORAGE_FS_H             // skip firmware/FileSystem.h
#define TIMER12_SERVICES_OTA_MANAGER_H   // skip firmware/OtaManager.h

// ============================================================================
// Part 4: Real ArduinoJson library (from .pio/libdeps/development)
//   MqttClient.cpp uses DynamicJsonDocument, StaticJsonDocument, JsonObject,
//   JsonArray, JsonPair, JsonDocument, deserializeJson, serializeJson,
//   DeserializationError.
//
//   Included via relative path from this header (located at
//   firmware/test/host/shims/). Three `..` traversals get us to firmware/,
//   then .pio/libdeps/development/ArduinoJson/src/ArduinoJson.h.
//
//   ArduinoJson's internal includes use angle brackets (<ArduinoJson/...>),
//   so the `src/` directory must be in the -I path. Add
//   `-I ../../.pio/libdeps/development/ArduinoJson/src` to the compile
//   command — without it, transitive includes like
//   <ArduinoJson/Array/ElementProxy.hpp> won't resolve.
//
//   ArduinoJson auto-detects Arduino String support via the ARDUINO macro.
//   Since we don't define ARDUINO (host build, not real Arduino), we MUST
//   explicitly enable ARDUINOJSON_ENABLE_ARDUINO_STRING before including
//   ArduinoJson.h — otherwise serializeJson/deserializeJson won't have
//   String specializations and MqttClient.cpp won't compile.
// ============================================================================
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include "../../../.pio/libdeps/development/ArduinoJson/src/ArduinoJson.h"

// ============================================================================
// Part 5: Core:: constants (replacement for firmware/Config.h)
//
// `inline const char*` (C++17 inline-variable) allows the variable to be
// defined in multiple TUs without ODR violations. Marked non-constexpr so
// host tests can mutate them at runtime (e.g., set
// OTA_ED25519_PUBLIC_KEY_HEX to a non-empty test value to exercise the
// OTA download path).
// ============================================================================
namespace Core {

  // ---------- FIRMWARE VERSION ----------
  inline const char* FIRMWARE_VERSION = "4.0.0";
  inline const char* BUILD_DATE = "HOST_TEST_BUILD";
  inline const char* DEVICE_MODEL = "ESP32-WROOM-32 Timer12 v4.0 HOST";
  constexpr uint8_t CONFIG_VERSION = 2;

  // ---------- CHANNELS ----------
  constexpr uint8_t NUM_CHANNELS = 12;
  constexpr uint8_t NUM_PIR = 4;
  constexpr uint8_t PIR_CHANNEL_OFFSET = 8;
  constexpr uint8_t MAX_SCHEDULES = 4;

  // ---------- PIN MAPPING ----------
  constexpr uint8_t RELAY_PINS[NUM_CHANNELS] = {
    13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27
  };
  constexpr uint8_t RELAY_ON = 0;
  constexpr uint8_t RELAY_OFF = 1;
  constexpr uint8_t PIR_PINS[NUM_PIR] = {34, 35, 36, 39};

  // ---------- I2C ----------
  constexpr uint8_t I2C_SDA = 32;
  constexpr uint8_t I2C_SCL = 33;
  constexpr uint32_t I2C_CLOCK = 400000;

  // ---------- WIFI AP (fallback mode) ----------
  inline const char* AP_SSID = "Timer12CH";
  constexpr uint8_t WIFI_CHANNEL = 6;
  constexpr uint8_t WIFI_MAX_CLIENTS = 4;
  constexpr bool WIFI_HIDDEN = false;
  constexpr int8_t WIFI_TX_POWER_DBM = 17;
  constexpr uint32_t AP_IP[] = {192, 168, 4, 1};

  // ---------- WIFI STA ----------
  inline const char* WIFI_CONFIG_PORTAL_SSID = "Timer12-Setup";
  inline const char* WIFI_CONFIG_PORTAL_PASSWORD = "";
  constexpr uint32_t WIFI_STA_TIMEOUT_MS = 15000;
  constexpr uint8_t WIFI_STA_MAX_RETRIES = 3;

  // ---------- NVS keys ----------
  inline const char* NVS_NAMESPACE = "timer12";
  inline const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
  inline const char* NVS_KEY_WIFI_PASS = "wifi_pass";
  inline const char* NVS_KEY_MQTT_PASS = "mqtt_pass";
  inline const char* NVS_KEY_DEVICE_PIN = "device_pin";
  inline const char* NVS_KEY_GAS_SECRET = "gas_secret";
  inline const char* NVS_KEY_JWT_SECRET = "jwt_secret";

  // ---------- MQTT BROKER ----------
  inline const char* MQTT_BROKER_HOST = "";
  inline uint16_t MQTT_BROKER_PORT = 0;
  inline const char* MQTT_BROKER_USERNAME = "";
  inline const char* MQTT_BROKER_PASSWORD = "";
  constexpr uint16_t MQTT_KEEPALIVE_SEC = 60;
  constexpr uint16_t MQTT_RECONNECT_DELAY_MS = 5000;
  constexpr uint16_t MQTT_STATUS_PUBLISH_INTERVAL_MS = 5000;
  constexpr uint16_t MQTT_BUFFER_SIZE = 4096;
  constexpr uint8_t MQTT_PASSWORD_LEN = 8;
  inline const char* MQTT_DEV_FALLBACK_HOST = "broker.hivemq.com";
  constexpr uint16_t MQTT_DEV_FALLBACK_PORT = 1883;
  inline const char* MQTT_ROOT_CA = "";

  // ---------- CORS ----------
  inline const char* ALLOWED_CORS_ORIGINS = "";

  // ---------- GOOGLE APPS SCRIPT (GAS) ----------
  inline const char* GAS_INSIGHTS_URL = "";
  constexpr uint32_t GAS_POST_INTERVAL_MS = 3600000;
  constexpr uint16_t GAS_TIMEOUT_MS = 30000;
  constexpr uint8_t GAS_SECRET_LEN = 32;
  constexpr uint8_t GAS_HMAC_LEN = 32;
  constexpr uint16_t GAS_MAX_BODY_SIZE = 16384;
  constexpr int32_t GAS_TIMESTAMP_TOLERANCE_SEC = 300;

  // ---------- PZEM ----------
  constexpr uint8_t PZEM_RX_PIN = 5;
  constexpr uint8_t PZEM_TX_PIN = 4;
  constexpr uint32_t PZEM_BAUD_RATE = 9600;
  constexpr uint8_t PZEM_MODBUS_ADDR = 0x01;
  constexpr uint16_t PZEM_READ_INTERVAL_MS = 1000;
  constexpr uint16_t PZEM_TIMEOUT_MS = 1000;
  constexpr float VOLTAGE_MAINS_V = 220.0f;

  // ---------- POWER ALARM THRESHOLDS ----------
  constexpr float ALARM_VOLTAGE_MIN = 190.0f;
  constexpr float ALARM_VOLTAGE_MAX = 250.0f;
  constexpr float ALARM_CURRENT_MAX = 8.0f;
  constexpr float ALARM_POWER_MAX = 1500.0f;
  constexpr float ALARM_PF_MIN = 0.70f;
  constexpr uint16_t ALARM_COOLDOWN_MS = 60000;

  // ---------- STORAGE LIMITS ----------
  constexpr size_t MAX_NAME_LEN = 20;
  constexpr size_t MAX_NAME_BUF = MAX_NAME_LEN + 1;
  constexpr size_t MAX_USER_LEN = 31;
  constexpr size_t MAX_USER_BUF = MAX_USER_LEN + 1;
  constexpr size_t MAX_TIME_STR = 5;
  constexpr size_t MAX_TIME_BUF = 6;
  constexpr size_t SALT_LEN = 16;
  constexpr size_t HASH_LEN = 32;
  constexpr size_t HASH_HEX_LEN = HASH_LEN * 2;
  constexpr size_t HASH_HEX_BUF_SIZE = HASH_HEX_LEN + 1;
  constexpr uint32_t PBKDF2_ITERATIONS = 50000;
  constexpr size_t MAX_BODY_SIZE = 16384;
  constexpr size_t MAX_AUDIT_LOG_SIZE = 8192;
  constexpr size_t MAX_ACTIVITY_LOG_ENTRIES = 200;

  // ---------- TIMING ----------
  constexpr unsigned long SAVE_DELAY_MS = 10000;
  constexpr unsigned long MAX_SAVE_DELAY_MS = 60000;
  constexpr unsigned long PIR_WARMUP_MS = 60000;
  constexpr unsigned long PIR_DEBOUNCE_INTERVAL_MS = 50;
  constexpr uint8_t PIR_DEBOUNCE_SAMPLES = 3;
  constexpr uint8_t PIR_DEBOUNCE_THRESHOLD = 2;
  constexpr unsigned long PIR_STUCK_TIMEOUT_MS = 1800000;
  constexpr unsigned long PIR_STUCK_COOLDOWN_MS = 300000;
  constexpr unsigned long RELAY_TICK_MS = 1000;

  // ---------- AUTH ----------
  constexpr uint8_t AUTH_FAIL_THRESHOLD_SHORT = 5;
  constexpr uint8_t AUTH_FAIL_THRESHOLD_LONG = 10;
  constexpr unsigned long AUTH_BLOCK_SHORT_MS = 60000;
  constexpr unsigned long AUTH_BLOCK_LONG_MS = 300000;
  constexpr size_t MAX_TRACKED_IPS = 8;
  constexpr uint16_t CSRF_TOKEN_LEN = 32;
  constexpr unsigned long CSRF_TOKEN_TTL_MS = 900000;
  constexpr uint32_t JWT_ACCESS_TTL_SECONDS = 900;
  constexpr uint32_t JWT_REFRESH_TTL_SECONDS = 604800;
  constexpr uint32_t JWT_TTL_SECONDS = JWT_ACCESS_TTL_SECONDS;
  constexpr size_t JWT_MAX_LEN = 512;
  constexpr size_t REFRESH_TOKEN_LEN = 32;
  constexpr uint8_t MAX_REFRESH_TOKENS = 4;
  constexpr unsigned long FACTORY_RESET_TOKEN_TTL_MS = 60000;

  // ---------- OTA ----------
  constexpr size_t OTA_MAX_BINARY_SIZE = 2 * 1024 * 1024;
  constexpr uint8_t ED25519_PUBLIC_KEY_LEN = 32;
  constexpr uint8_t ED25519_SIGNATURE_LEN = 64;
  constexpr size_t SHA256_HEX_LEN = 64;
  // Mutable — host tests can set non-empty values to exercise OTA download path.
  inline const char* OTA_ED25519_PUBLIC_KEY_HEX = "";
  inline const char* OTA_HTTPS_ROOT_CA = "";
  inline const char* OTA_ALLOWED_HOSTS = "";

  // ---------- FILE PATHS ----------
  inline const char* PATH_CONFIG_JSON = "/config.json";
  inline const char* PATH_CONFIG_BAK = "/config.bak";
  inline const char* PATH_CONFIG_TMP = "/config.tmp";
  inline const char* PATH_SCHEDULE_JSON = "/schedule.json";
  inline const char* PATH_SCHEDULE_BAK = "/schedule.bak";
  inline const char* PATH_SCHEDULE_TMP = "/schedule.tmp";
  inline const char* PATH_SCHEDULE_BAK_TMP = "/schedule.bak.tmp";
  inline const char* PATH_AUDIT_LOG = "/audit.log";
  inline const char* PATH_AUDIT_LOG_OLD = "/audit.log.old";
  inline const char* PATH_ACTIVITY_LOG = "/activity.log";
  inline const char* PATH_ACTIVITY_LOG_OLD = "/activity.log.old";

  // ---------- DEFAULTS ----------
  inline const char* DEFAULT_TIMEZONE = "Asia/Jakarta";
  inline const char* JWT_SECRET_DEFAULT = "";
} // namespace Core

// ============================================================================
// Part 6: Core:: types (replacement for firmware/Types.h)
// ============================================================================
namespace Core {

  // ---------- SCHEDULE ----------
  struct Schedule {
    char onTime[MAX_TIME_BUF];
    char offTime[MAX_TIME_BUF];
    uint16_t onMin;
    uint16_t offMin;
    uint8_t dayMask;
    bool enabled;
  };

  // ---------- CHANNEL ----------
  struct Channel {
    char name[MAX_NAME_BUF];
    Schedule sched[MAX_SCHEDULES];
    uint8_t schedCount;
    bool manualState;
    bool modeAuto;
    bool pirEnabled;
    uint16_t pirHoldTime;
    uint32_t energyWh;
    uint16_t wattage;
    unsigned long lastOnMs;
  };

  // ---------- USER CONFIG (auth) ----------
  struct UserConfig {
    char wwwUser[MAX_USER_BUF];
    char passHashHex[HASH_HEX_BUF_SIZE];
    uint8_t salt[SALT_LEN];
    uint16_t iterations;
  };

  // ---------- LOG TYPES ----------
  enum class LogType : uint8_t {
    RelayOn = 0,
    RelayOff,
    PirTrigger,
    Login,
    Logout,
    Error,
    Restart,
    Ota,
    ConfigChange,
    FactoryReset,
    TimeSync,
    AuthFail,
  };

  // ---------- ACTIVITY LOG ENTRY ----------
  struct ActivityLogEntry {
    uint32_t id;
    uint32_t timestamp;
    LogType type;
    int8_t channelId;
    char message[96];
  };

  // ---------- RELAY SOURCE ----------
  enum class RelaySource : uint8_t {
    Off = 0,
    Manual,
    Schedule,
    Pir,
  };

  // ---------- AUTH ATTEMPT ----------
  struct AuthAttempt {
    uint32_t ip;
    uint8_t failCount;
    unsigned long lastFailTime;
    unsigned long blockUntil;
    bool active;
  };

  // ---------- PIR RUNTIME STATE ----------
  struct PirState {
    bool motionNow;
    bool everTriggered;
    unsigned long lastMotion;
    unsigned long highSince;
    bool stuckAlerted;
    unsigned long stuckCooldownUntil;
    unsigned long lastSampleTime;
    uint8_t sampleHistory[3];
    uint8_t sampleIdx;
    uint32_t triggerCountToday;
  };

  // ---------- SYSTEM METRICS ----------
  struct SystemMetrics {
    uint32_t bootTime;
    uint32_t lastDailyResetDay;
    uint32_t errorsToday;
    uint32_t pirTriggersToday[NUM_PIR];
    bool online;
  };

} // namespace Core

// ============================================================================
// Part 7: Core:: globals (extern declarations — replaces firmware/Globals.h)
//
// Definitions are normally in firmware_v4.ino (the main sketch). Since host
// tests don't compile the .ino, definitions are provided by the test driver
// (e.g., MqttClientTest.cpp). When compiling ONLY MqttClient.cpp +
// TransactionJournal.cpp + JournalRecord.cpp to object files (no link),
// extern declarations suffice — the linker is not invoked.
// ============================================================================
namespace Core {

  inline Channel channels[NUM_CHANNELS];
  inline bool relayState[NUM_CHANNELS];
  inline RelaySource relaySource[NUM_CHANNELS];
  inline PirState pirState[NUM_PIR];
  inline UserConfig userConfig;
  inline SystemMetrics metrics;
  inline bool timeValid;
  inline bool scheduleDirty;
  inline bool firstDirtySet;
  inline unsigned long lastSaveTime;
  inline unsigned long firstDirtyTime;
  inline unsigned long pirStartupTime;
  inline char csrfToken[CSRF_TOKEN_LEN + 1];
  inline unsigned long csrfTokenTime;
  inline char apPassword[33];
  inline char deviceName[33] = {};
  extern char timezone[40];
  inline char timezone[40] = {};
  inline char jwtSecret[65] = {};
  inline char factoryResetToken[33] = {};
  extern char jwtSecret[65];
  inline AuthAttempt authAttempts[MAX_TRACKED_IPS];
  extern char factoryResetToken[33];
  inline unsigned long factoryResetTokenTime;

} // namespace Core

// ============================================================================
// Part 8: ESP32 system symbols
// ============================================================================

// delay() — Arduino function, no-op on host
inline void delay(unsigned long /*ms*/) {}

// ESP class — Arduino ESP32 framework. restart() sets a flag instead of
// actually restarting the test process (otherwise tests would kill g++).
namespace host_shim {
  inline bool g_espRestartCalled = false;
}

class EspClass {
public:
  uint32_t getFreeHeap() { return 100 * 1024; }
  void restart() {
    // Set flag instead of restarting — test verifies behavior
    host_shim::g_espRestartCalled = true;
  }
};
static inline EspClass ESP;

// esp_task_wdt — ESP-IDF watchdog (no-ops on host)
extern "C" {
  inline void esp_task_wdt_reset() {}
  inline void esp_task_wdt_init(uint32_t, bool) {}
  inline void esp_task_wdt_add(void*) {}
  inline void esp_task_wdt_delete(void*) {}
}

// arduino_event types (used by WifiManager.h's onEvent signature)
typedef int arduino_event_id_t;
typedef int arduino_event_info_t;

// ============================================================================
// Part 9: mbedtls/sha256.h shim (real SHA-256 via OpenSSL EVP API)
//
// Uses EVP_DigestInit_ex / EVP_DigestUpdate / EVP_DigestFinal_ex — the
// recommended OpenSSL 3.0 API (the legacy SHA256_Init/Update/Final functions
// are deprecated and break -Werror=deprecated-declarations).
// ============================================================================
typedef struct mbedtls_sha256_context {
  EVP_MD_CTX* ctx;
} mbedtls_sha256_context;

inline void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
  if (ctx) ctx->ctx = EVP_MD_CTX_new();
}

inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int /*is224*/) {
  if (!ctx || !ctx->ctx) return -1;
  return (EVP_DigestInit_ex(ctx->ctx, EVP_sha256(), nullptr) == 1) ? 0 : -1;
}

inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx,
                                  const uint8_t* data, size_t len) {
  if (!ctx || !ctx->ctx) return -1;
  return (EVP_DigestUpdate(ctx->ctx, data, len) == 1) ? 0 : -1;
}

inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t* out) {
  if (!ctx || !ctx->ctx) return -1;
  unsigned int outLen = 0;
  return (EVP_DigestFinal_ex(ctx->ctx, out, &outLen) == 1) ? 0 : -1;
}

inline void mbedtls_sha256_free(mbedtls_sha256_context* ctx) {
  if (ctx && ctx->ctx) {
    EVP_MD_CTX_free(ctx->ctx);
    ctx->ctx = nullptr;
  }
}

// ============================================================================
// Part 10: Network framework stubs
//
// These classes are normally found via `#include <PubSubClient.h>` etc.
// (angle brackets → -I shims → finds shims/PubSubClient.h which is an EMPTY
// file with just `#pragma once`). Since the empty shim headers don't provide
// the class definitions, we define them HERE. When MqttClient.cpp does
// `#include <PubSubClient.h>`, the empty shim is a no-op and our class
// definition (already visible via force-include) takes effect.
// ============================================================================

typedef uint8_t byte;

class IPAddress {
public:
  IPAddress() : addr_(0) {}
  IPAddress(uint32_t a) : addr_(a) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
    : addr_((uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d) {}
  operator uint32_t() const { return addr_; }
private:
  uint32_t addr_;
};

class WiFiClient {
public:
  WiFiClient() {}
  WiFiClient(const WiFiClient&) {}
  virtual ~WiFiClient() {}
  int available() { return 0; }
  int read() { return -1; }
  int readBytes(uint8_t* /*buf*/, size_t /*len*/) { return 0; }
  void stop() {}
  bool connected() { return false; }
  operator bool() const { return false; }
};

class WiFiClientSecure : public WiFiClient {
public:
  WiFiClientSecure() {}
  void setCACert(const char* /*ca*/) {}
  void setInsecure() {}
};

// ---------------------------------------------------------------------------
// PubSubClient stub — minimal MQTT client API used by MqttClient.cpp.
//
// Test-controllable state: instead of static globals (which would conflict
// across multiple test files), the connection state and publish success are
// per-instance fields with sensible defaults (connected=true, publish OK).
// Host tests can mutate these via the `private public` trick or by directly
// constructing instances.
// ---------------------------------------------------------------------------
class PubSubClient {
public:
  PubSubClient() {}
  PubSubClient(WiFiClient&) {}

  // Configuration — all no-ops (MqttClient.cpp calls these in begin())
  void setClient(WiFiClient&) {}
  void setClient(WiFiClientSecure&) {}
  void setServer(const char*, uint16_t) {}
  void setKeepAlive(uint16_t) {}
  void setBufferSize(uint16_t) {}
  template <typename F>
  void setCallback(F /*cb*/) {}

  // Connection state — per-instance, default: connected
  bool connected() { return _connected; }

  bool connect(const char* /*clientId*/,
               const char* /*user*/, const char* /*pass*/,
               const char* /*willTopic*/, uint8_t /*willQos*/,
               bool /*willRetain*/, const char* /*willMsg*/) {
    return _connected;
  }

  bool subscribe(const char* /*topic*/, uint8_t /*qos*/) { return true; }

  bool publish(const char* topic, const char* payload, bool /*retain*/) {
    (void)topic; (void)payload;
    return _publishOk;
  }
  bool publish(const char* topic, const uint8_t* payload, unsigned int len, bool /*retain*/) {
    (void)topic; (void)payload; (void)len;
    return _publishOk;
  }

  void loop() {}
  int state() { return 0; }

  // Test-controllable fields (mutable — tests can override via friend access
  // or by direct write if private is made public via #define).
  bool _connected = true;
  bool _publishOk = true;
};

// ---------------------------------------------------------------------------
// HTTPClient stub (used by _downloadAndVerifyOta for HTTPS firmware download)
// ---------------------------------------------------------------------------
#define HTTPC_STRICT_FOLLOW_REDIRECTS 1
#define HTTP_CODE_OK 200

class HTTPClient {
public:
  HTTPClient() {}
  void setFollowRedirects(int) {}
  void setTimeout(uint16_t) {}
  void setConnectTimeout(uint16_t) {}
  bool begin(WiFiClientSecure&, const String&) { return false; }
  int sendRequest(const char*) { return 0; }
  int GET() { return 0; }
  int getSize() { return 0; }
  WiFiClient* getStreamPtr() { return nullptr; }
  // MqttClient.cpp's OTA download loop polls http.connected() to detect
  // stream end. Returns false (no stream open) — the loop exits immediately,
  // causing _downloadAndVerifyOta to fail at the "incomplete download" check.
  // This matches the Update.begin()-fails path: OTA cannot succeed on host.
  bool connected() { return false; }
  void end() {}
};

// ---------------------------------------------------------------------------
// Update stub — OTA flash API
//
// Default behavior: begin() returns false (OTA cannot start). This causes
// _downloadAndVerifyOta to fail at the Update.begin() step, which is BEFORE
// the mbedtls/Ed25519 calls. Tests that exercise the OTA download path will
// hit this failure (or the earlier OTA_HTTPS_ROOT_CA empty check).
// ---------------------------------------------------------------------------
class UpdateClass {
public:
  bool begin(size_t /*size*/) { return false; }
  size_t write(uint8_t* /*buf*/, size_t len) { return len; }
  bool end(bool /*evenIfRemaining*/ = false) { return false; }
  bool isFinished() { return false; }
  bool isRunning() { return false; }
  void abort() {}
  uint8_t getError() { return 0; }
};
static inline UpdateClass Update;

// ---------------------------------------------------------------------------
// RTClib stub (RtcDriver.h includes <RTClib.h> — but we skip RtcDriver.h;
// this stub is provided for completeness in case it's transitively reached).
// ---------------------------------------------------------------------------
class RTC_DS3231 {
public:
  bool begin() { return true; }
  bool now() { return true; }
};

// ---------------------------------------------------------------------------
// WebServer stub (AuthManager.h includes <WebServer.h>)
// ---------------------------------------------------------------------------
class WebServer {
public:
  WebServer(int /*port*/ = 80) {}
  void send(int, const String&, const String&) {}
  void send(int, const String&, const uint8_t*, size_t) {}
  void sendHeader(const String&, const String&) {}
  String uri() { return ""; }
  String arg(const String&) { return ""; }
  String arg(int) { return ""; }
  bool hasHeader(const String&) { return false; }
  String header(const String&) { return ""; }
  bool hasArg(const String&) { return false; }
  bool hasArg(int) { return false; }
  int args() { return 0; }
  String argName(int) { return ""; }
  void stop() {}
};

// ============================================================================
// Part 11: Utils:: namespace (replacement for firmware/Crypto.h, Json.h, Crc.h)
//   Provides REAL SHA-256 via OpenSSL EVP, plus stubs for functions not
//   exercised by MqttClient.cpp's test paths.
// ============================================================================
namespace Utils {

  // ---- Crypto.h ----

  // Constant-time memory compare — stub (not exercised by MqttClient tests).
  inline bool constantTimeMemEquals(const volatile uint8_t*, const volatile uint8_t*, size_t) {
    return false;
  }

  // Pseudo-random bytes (deterministic — sufficient for host tests).
  inline void generateRandomBytes(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i & 0xFF);
  }

  // Hex encoding (matches Crypto.cpp impl). Decoding is a stub.
  inline void bytesToHex(const uint8_t* in, size_t len, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
      out[i * 2]     = hex[(in[i] >> 4) & 0x0F];
      out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
  }
  inline bool hexToBytes(const char*, uint8_t*, size_t) { return false; }

  // REAL SHA-256 (OpenSSL EVP) — matches firmware's mbedtls-based impl.
  // Used heavily by _computeCommandHash and the OTA download path.
  inline String sha256Hex(const String& data) {
    uint8_t hash[32];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.c_str(), data.length());
    unsigned int outLen = 0;
    EVP_DigestFinal_ex(ctx, hash, &outLen);
    EVP_MD_CTX_free(ctx);
    char hex[65];
    bytesToHex(hash, 32, hex);
    return String(hex);
  }

  // PBKDF2 / HMAC / JWT — stubs (not used by MqttClient).
  inline bool pbkdf2HmacSha256(const char*, size_t, const uint8_t*, size_t, uint16_t, uint8_t*) {
    return false;
  }
  inline bool hmacSha256(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) {
    return false;
  }
  inline String base64urlEncode(const uint8_t*, size_t) { return ""; }
  inline String base64urlEncode(const String&) { return ""; }
  inline String jwtSign(const String&, const String&, uint32_t) { return ""; }
  inline bool jwtVerify(const String&, const String&, String&) { return false; }
  inline String generateToken(size_t) { return ""; }

  // Ed25519 stub — returns false (forces OTA fail-closed).
  // Real verification requires mbedtls Ed25519 support; tests don't reach
  // this path because the OTA flow fails earlier on empty OTA_ED25519_PUBLIC_KEY_HEX.
  inline bool ed25519VerifyHash(const char*, const char*, const uint8_t*, size_t) {
    return false;
  }

  // ---- Json.h ----

  // Parse "HH:MM" → minutes since midnight (matches Json.cpp impl).
  inline bool parseMinutes(const char* str, uint16_t& minutes) {
    if (!str) return false;
    size_t len = strlen(str);
    if (len != 5) return false;
    if (str[2] != ':') return false;
    for (int i : {0, 1, 3, 4}) {
      if (str[i] < '0' || str[i] > '9') return false;
    }
    int h = (str[0] - '0') * 10 + (str[1] - '0');
    int m = (str[3] - '0') * 10 + (str[4] - '0');
    if (h > 23 || m > 59) return false;
    minutes = (uint16_t)(h * 60 + m);
    return true;
  }

  // Basic date validation (matches Json.cpp impl).
  inline bool isValidDate(int y, int m, int d) {
    if (y < 2000 || y > 2100) return false;
    if (m < 1 || m > 12) return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxDay = days[m - 1];
    if (m == 2) {
      bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
      if (leap) maxDay = 29;
    }
    return d >= 1 && d <= maxDay;
  }

  // Stubs for helpers declared in Json.h but not used by MqttClient.cpp.
  inline uint32_t computeDocCRC(JsonDocument&) { return 0; }
  inline void appendCRC(JsonDocument&) {}
  inline bool isPasswordStrong(const String&) { return true; }
  inline String sanitizeForLog(const String&, size_t = 80) { return ""; }

  // ---- Crc.h ----

  // CRC-32 (zlib polynomial) — inline stub matching the firmware Crc.cpp impl.
  inline uint32_t calculateCRC(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
      crc ^= data[i];
      for (int j = 0; j < 8; j++) {
        crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
      }
    }
    return ~crc;
  }

} // namespace Utils

// ============================================================================
// Part 12: Services:: namespace (replaces LogService.h, RelayEngine.h,
//          Scheduler.h, AuthManager.h)
//
// NOTE: TransactionJournal + journal come from the real TransactionJournal.cpp
// (compiled alongside MqttClient.cpp). We deliberately do NOT skip
// TransactionJournal.h's include guard — the real header is processed so the
// class declaration matches the compiled .cpp.
//
// MqttClient is declared in firmware/MqttClient.h (NOT skipped) and defined
// in MqttClient.cpp (which is what we're compiling). The extern `mqtt`
// declaration is in MqttClient.h.
// ============================================================================
namespace Services {

  // ---- LogService.h ----
  class LogServiceClass {
  public:
    void begin() {}
    void append(Core::LogType /*type*/, const char* /*msg*/, int8_t /*channelId*/) {}
    void append(Core::LogType /*type*/, const String& /*msg*/, int8_t /*channelId*/) {}
    void appendAudit(const String& /*entry*/) {}
    String getAuditLogText(size_t /*maxBytes*/ = 8192) { return ""; }
    String getActivityLogJson(int /*limit*/ = 200, int /*filterType*/ = -1, int /*filterChannel*/ = 0) {
      return "[]";
    }
  };
  inline LogServiceClass Log;

  // ---- RelayEngine.h ----
  // Stub updates Core::relayState / Core::relaySource / Core::channels[idx].modeAuto
  // directly (matches production side-effect of RelayEngine::setManual/setMode).
  class RelayEngine {
  public:
    void tick() {}
    void forceRefresh() {}
    void setManual(uint8_t idx, bool on) {
      if (idx < Core::NUM_CHANNELS) {
        Core::relayState[idx] = on;
        Core::relaySource[idx] = on ? Core::RelaySource::Manual : Core::RelaySource::Off;
      }
    }
    void setMode(uint8_t idx, bool autoMode) {
      if (idx < Core::NUM_CHANNELS) Core::channels[idx].modeAuto = autoMode;
    }
    void toggle(uint8_t idx) {
      if (idx < Core::NUM_CHANNELS) Core::relayState[idx] = !Core::relayState[idx];
    }
  };
  inline RelayEngine relayEngine;

  // ---- Scheduler.h ----
  class Scheduler {
  public:
    bool isScheduleActive(const Core::Schedule&, uint16_t, int) { return false; }
    bool isChannelScheduled(uint8_t, uint16_t, int) { return false; }
    void save(bool /*force*/ = false) {}
  };
  inline Scheduler scheduler;

  // ---- AuthManager.h ----
  // MqttClient.cpp does NOT directly use Services::auth, but the real
  // firmware/AuthManager.h is included (transitively) — stub provided for
  // compilation completeness.
  class AuthManager {
  public:
    void begin() {}
    void generateCsrfToken() {}
    String getCsrfToken() const { return ""; }
    bool checkCsrfToken(WebServer&) const { return true; }
    bool login(const String&, const String&, String&, String&, String&, uint32_t&, uint32_t) {
      return false;
    }
    bool refreshTokens(const String&, String&, String&, String&, uint32_t&) {
      return false;
    }
    bool checkAuth(WebServer&) { return true; }
    void logout(WebServer&) {}
    bool isRateLimited(uint32_t) const { return false; }
    void recordAuthFailure(uint32_t) {}
    void recordAuthSuccess(uint32_t) {}
    bool changePassword(const String&, const String&) { return false; }
    String generateFactoryResetToken() { return ""; }
    bool consumeFactoryResetToken(const String&) { return false; }
  };
  inline AuthManager auth;

} // namespace Services

// ============================================================================
// Part 13: Drivers:: namespace (replaces RtcDriver.h, RelayDriver.h,
//          PirDriver.h, PzemDriver.h)
// ============================================================================
namespace Drivers {

  // ---- RtcDriver.h ----
  // Backed by std::time() — sufficient for host tests (no real RTC hardware).
  class RtcDriver {
  public:
    bool begin() { _initialized = true; return true; }
    bool isValid() { return _initialized; }
    uint32_t getUnixTime() { return (uint32_t)std::time(nullptr); }
    void getDateTime(int& y, int& m, int& d, int& h, int& mi, int& s, int& weekday) {
      std::time_t t = std::time(nullptr);
      std::tm* lt = std::localtime(&t);
      y = lt->tm_year + 1900;
      m = lt->tm_mon + 1;
      d = lt->tm_mday;
      h = lt->tm_hour;
      mi = lt->tm_min;
      s = lt->tm_sec;
      weekday = lt->tm_wday;
    }
    void adjust(int, int, int, int, int, int) {}
    void adjust(uint32_t) {}
    int getWeekdayIndex() { return 0; }
    String formatTime() { return "00:00:00"; }
    String formatDate() { return "2024-01-01"; }
  private:
    bool _initialized = true;
  };
  inline RtcDriver rtc;

  // ---- RelayDriver.h ----
  // Stub updates Core::relayState[idx] directly. readLogicalState() returns
  // the in-RAM state (matches "what we last wrote"). Tests can simulate a
  // GPIO mismatch by overwriting Core::relayState[idx] before the call.
  class RelayDriver {
  public:
    void begin() {}
    void setChannel(uint8_t idx, bool on) {
      if (idx < Core::NUM_CHANNELS) Core::relayState[idx] = on;
    }
    bool getState(uint8_t idx) const {
      return (idx < Core::NUM_CHANNELS) ? Core::relayState[idx] : false;
    }
    void allOff() {
      for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) Core::relayState[i] = false;
    }
    bool readLogicalState(uint8_t idx) const {
      return (idx < Core::NUM_CHANNELS) ? Core::relayState[idx] : false;
    }
  };
  inline RelayDriver relay;

  // ---- PirDriver.h ----
  // testTrigger() updates Core::pirState[idx] (matches production side-effect:
  // successful test trigger increments triggerCountToday + sets motionNow).
  class PirDriver {
  public:
    void begin() {}
    void tick() {}
    bool readDebounced(uint8_t) { return false; }
    bool isMotion(uint8_t idx) const {
      return (idx < Core::NUM_PIR) ? Core::pirState[idx].motionNow : false;
    }
    bool isStuck(uint8_t idx) const {
      return (idx < Core::NUM_PIR) ? Core::pirState[idx].stuckAlerted : false;
    }
    void testTrigger(uint8_t idx) {
      if (idx < Core::NUM_PIR) {
        Core::pirState[idx].triggerCountToday++;
        Core::pirState[idx].motionNow = true;
        Core::pirState[idx].lastMotion = millis();
        Core::pirState[idx].everTriggered = true;
      }
    }
    void resetAll() {}
    void resetDailyCounters() {}
  };
  inline PirDriver pir;

  // ---- PzemDriver.h ----
  // Default behavior: isAvailable() returns false → publishStatus skips the
  // PZEM data block entirely (matches "PZEM not connected" production state).
  struct PzemData {
    float voltage, current, power, energy, frequency, powerFactor;
    bool alarm;
  };
  struct PzemDerived {
    float apparentPower, reactivePower;
  };
  struct PzemDailyStats {
    float energyStartKwh, energyTodayKwh, voltageMin, voltageMax;
    float currentMax, powerMax, powerAvg, powerSum;
    uint32_t sampleCount;
    uint8_t lastResetDay;
  };
  struct PzemAlarms {
    bool undervoltage, overvoltage, overcurrent, overpower, lowPowerFactor;
    unsigned long lastUnderVAlarmMs, lastOverVAlarmMs, lastOverIAlarmMs;
    unsigned long lastOverPAlarmMs, lastLowPfAlarmMs;
  };

  class PzemDriver {
  public:
    bool begin() { return false; }
    void tick() {}
    bool isAvailable() const { return _available; }
    PzemData getData() const { return _data; }
    PzemDerived getDerived() const { return _derived; }
    PzemDailyStats getDailyStats() const { return _daily; }
    PzemAlarms getAlarms() const { return _alarms; }
    float getVoltage() const { return _data.voltage; }
    float getCurrent() const { return _data.current; }
    float getPower() const { return _data.power; }
    float getEnergy() const { return _data.energy; }
    float getFrequency() const { return _data.frequency; }
    float getPowerFactor() const { return _data.powerFactor; }
    bool hasAlarm() const { return _data.alarm; }
    float getApparentPower() const { return _derived.apparentPower; }
    float getReactivePower() const { return _derived.reactivePower; }
    float getEnergyToday() const { return _daily.energyTodayKwh; }
    float getVoltageMin() const { return _daily.voltageMin; }
    float getVoltageMax() const { return _daily.voltageMax; }
    float getCurrentMax() const { return _daily.currentMax; }
    float getPowerMax() const { return _daily.powerMax; }
    float getPowerAvg() const {
      return _daily.sampleCount > 0 ? _daily.powerSum / _daily.sampleCount : 0;
    }
    bool resetEnergy() { return true; }
    void resetDailyStats() {}
  private:
    bool _available = false;
    PzemData _data = {0, 0, 0, 0, 0, 0, false};
    PzemDerived _derived = {0, 0};
    PzemDailyStats _daily = {0, 0, 999, 0, 0, 0, 0, 0, 0, 255};
    PzemAlarms _alarms = {false, false, false, false, false, 0, 0, 0, 0, 0};
  };
  inline PzemDriver pzem;

} // namespace Drivers

// ============================================================================
// Part 14: Storage:: namespace (replaces ConfigStore.h, FileSystem.h)
// ============================================================================
namespace Storage {

  // ---- ConfigStore.h ----
  // MqttClient.cpp uses markDirty() and saveDeviceConfig(). Stub provided.
  class ConfigStore {
  public:
    void loadUserConfig() {}
    void saveUserConfig() {}
    void initDefaultUserConfig() {}
    void loadSchedule() {}
    void saveSchedule(bool /*force*/ = false) {}
    void resetChannels() {}
    void markDirty() {}
    void clearDirty() {}
    void loadDeviceConfig() {}
    void saveDeviceConfig() {}
    String exportAll() { return "{}"; }
    bool importAll(const String&) { return false; }
  };
  inline ConfigStore config;

  // ---- FileSystem.h ----
  // LittleFS wrapper. MqttClient.cpp does NOT directly use Storage::fs, but
  // provided for completeness. The real FileSystem.h returns `File` (from
  // LittleFS) — we omit that method to avoid needing a File shim.
  class FileSystem {
  public:
    bool begin() { return true; }
    void cleanupTempFiles() {}
    bool exists(const char*) { return false; }
    bool remove(const char*) { return false; }
    bool rename(const char*, const char*) { return false; }
    bool atomicWrite(const char*, const String&) { return false; }
    String readAll(const char*) { return ""; }
  };
  inline FileSystem fs;

} // namespace Storage

// ============================================================================
// Part 15: TimerNet:: namespace (replaces WifiManager.h)
//   MqttClient.cpp uses TimerNet::wifi.getMacAddress(), .isConnected(),
//   and .getRssi(). Stub returns deterministic test values.
// ============================================================================
namespace TimerNet {

  enum class WifiMode {
    STA,
    AP_CONFIG,
    AP_FALLBACK,
    NONE
  };

  class WifiManager {
  public:
    bool begin() { return true; }
    void onEvent(arduino_event_id_t, arduino_event_info_t) {}
    String getApPassword() const { return ""; }
    IPAddress getLocalIp() const { return IPAddress(); }
    uint8_t getClientCount() const { return 0; }
    int getRssi() const { return -50; }
    void generateApPassword() {}
    WifiMode getMode() const { return WifiMode::STA; }
    String getMacAddress() const { return "AABBCCDDEEFF"; }
    bool isConnected() const { return true; }
    String getMqttPassword() const { return "test1234"; }
    String getDevicePin() const { return "123456"; }
    String getGasSecretHex() const { return ""; }
    void openConfigPortal() {}
  };
  inline WifiManager wifi;

} // namespace TimerNet

// ============================================================================
// Part 16: Web:: namespace (replaces HttpServer.h)
//   MqttClient.cpp does NOT include HttpServer.h or use Web::server/http,
//   but we provide extern declarations so a test driver can link against
//   them if needed. (HttpServer.h is pulled in transitively by Common.h
//   when other handlers are compiled, so the extern declarations must
//   resolve at link time.)
// ============================================================================
namespace Web {

  class HttpServer {
  public:
    void begin() {}
    void handleClient() {}
  };
  inline HttpServer server;
  inline WebServer http;

} // namespace Web

#endif // HOST_SHIM_MQTTCLIENT_DEPS_H
