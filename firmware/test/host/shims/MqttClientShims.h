// =============================================================================
// MqttClientShims.h — Host-side shims for compiling firmware/MqttClient.cpp
// =============================================================================
// AUDITOR P2-2 F-P0-1 GATE (correction 3): tests must call the REAL
// MqttClient::_handleCommand() and MqttClient::_handleOta() — NOT a replicated
// routing model. The previous CommandRoutingTest.cpp duplicated the routing
// logic, which the auditor rejected.
//
// PROBLEM
//   MqttClient.cpp pulls in 25+ ESP32 / firmware headers, none of which
//   compile on the host. To compile the REAL MqttClient.cpp + call the REAL
//   private methods _handleCommand() and _handleOta(), we must shim every
//   ESP32 / firmware dependency.
//
// SOLUTION
//   This header is force-included via `g++ -include MqttClientShims.h` in
//   EVERY translation unit (MqttClientTest.cpp + MqttClient.cpp +
//   TransactionJournal.cpp + JournalRecord.cpp). That way, when Types.h /
//   Globals.h are processed inside MqttClient.cpp's TU, ALL of the Core::
//   constants, ESP32 system stubs, and framework stubs are already visible.
//
//   Firmware namespace stubs (Drivers::, Services::, Storage::, TimerNet::,
//   Utils::) are provided in SEPARATE stub header files in this same directory
//   (shims/LogService.h, shims/RelayEngine.h, etc.). When MqttClient.cpp
//   does `#include "LogService.h"`, the `-I shims` include path resolves to
//   our stub FIRST, overriding the real firmware/LogService.h. This avoids
//   class-redefinition conflicts that would occur if we tried to declare
//   these classes here AND in the real firmware headers.
//
//   The existing shims (Arduino.h, Config.h, Preferences.h, esp_crc.h) are
//   included as the foundation. This header ADDS:
//     - All remaining Core:: constants (NUM_CHANNELS, MQTT_*, OTA_*, etc.)
//     - ESP32 system stubs (delay, ESP, esp_task_wdt, mbedtls_sha256_*)
//     - Framework stubs (WiFiClient, WiFiClientSecure, PubSubClient,
//       HTTPClient, Update, WebServer, IPAddress, RTC_DS3231)
//     - Real SHA-256 via OpenSSL libcrypto (used by stub Utils::sha256Hex)
//
// IMPORTANT
//   - This file is ONLY compiled for host tests. The ESP32 build uses the
//     real headers from the arduino-esp32 framework.
//   - The existing shims (Arduino.h, Config.h, Preferences.h, esp_crc.h)
//     are NOT modified — they are included as-is.
//   - Production source files (MqttClient.cpp, MqttClient.h,
//     TransactionJournal.cpp/.h, JournalRecord.cpp/.h) are NOT modified.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_MQTTCLIENT_SHIMS_H
#define HOST_SHIM_MQTTCLIENT_SHIMS_H

// === Existing shims (foundation — Arduino.h's String/Serial/min/millis,
//     Config.h's Core::NVS_NAMESPACE, Preferences NVS, esp_crc32_le) ===
//
// NOTE: We do NOT include the existing shims/Config.h here. Instead we define
// `TIMER12_CORE_CONFIG_H` below so the REAL firmware/Config.h (which would be
// found first via the current-file directory when MqttClient.cpp does
// `#include "Config.h"`) is SKIPPED entirely. This shim becomes the SOLE
// provider of Core:: constants — which is what we want, because we need
// some constants (e.g., OTA_ED25519_PUBLIC_KEY_HEX) to be MUTABLE so tests
// can change them at runtime.
#include "Arduino.h"
#include "Preferences.h"
#include "esp_crc.h"

// Skip the REAL firmware/Config.h — its include guard is TIMER12_CORE_CONFIG_H.
// Defining this macro before any #include "Config.h" causes the real Config.h
// to be a no-op when reached, leaving THIS shim as the sole provider of Core::.
#define TIMER12_CORE_CONFIG_H

// === Standard library (needed for stubs below) ===
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <functional>

// OpenSSL for REAL SHA-256 (host has libssl-dev installed)
#include <openssl/sha.h>

// =============================================================================
// Core::Config constants (the existing Config.h shim only has NVS_NAMESPACE;
// MqttClient.cpp needs ~30 more constants — added here)
//
// NOTE: `inline const char*` is C++17 inline-variable syntax — allows the
// variable to be defined in multiple TUs (one per .cpp that force-includes
// this header) without ODR violations. The variables are MUTABLE so tests
// can change them at runtime (e.g., set OTA_ED25519_PUBLIC_KEY_HEX to a
// non-empty test value to exercise the OTA download path).
// =============================================================================
namespace Core {

  // ---------- FIRMWARE VERSION ----------
  inline const char* FIRMWARE_VERSION = "4.0.0";
  inline const char* BUILD_DATE = "TEST_BUILD";

  // ---------- CHANNELS ----------
  constexpr uint8_t NUM_CHANNELS = 12;
  constexpr uint8_t NUM_PIR = 4;
  constexpr uint8_t PIR_CHANNEL_OFFSET = 8;
  constexpr uint8_t MAX_SCHEDULES = 4;

  constexpr uint8_t RELAY_PINS[NUM_CHANNELS] = {
    13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27
  };
  constexpr uint8_t RELAY_ON = 0;
  constexpr uint8_t RELAY_OFF = 1;
  constexpr uint8_t PIR_PINS[NUM_PIR] = {34, 35, 36, 39};

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

  // ---------- TIMING ----------
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

  // ---------- MQTT BROKER (test defaults: empty — fail-closed) ----------
  inline const char* MQTT_BROKER_HOST = "";
  inline uint16_t MQTT_BROKER_PORT = 0;
  inline const char* MQTT_BROKER_USERNAME = "";
  inline const char* MQTT_BROKER_PASSWORD = "";
  inline uint16_t MQTT_KEEPALIVE_SEC = 60;
  inline uint16_t MQTT_RECONNECT_DELAY_MS = 5000;
  inline uint16_t MQTT_STATUS_PUBLISH_INTERVAL_MS = 5000;
  inline uint16_t MQTT_BUFFER_SIZE = 4096;
  inline uint8_t MQTT_PASSWORD_LEN = 8;
  inline const char* MQTT_DEV_FALLBACK_HOST = "broker.hivemq.com";
  inline uint16_t MQTT_DEV_FALLBACK_PORT = 1883;
  inline const char* MQTT_ROOT_CA = "";

  // ---------- CORS ----------
  inline const char* ALLOWED_CORS_ORIGINS = "";

  // ---------- GAS (referenced in real Config.h, not used by MqttClient) ----------
  inline const char* GAS_INSIGHTS_URL = "";
  constexpr uint32_t GAS_POST_INTERVAL_MS = 3600000;
  constexpr uint16_t GAS_TIMEOUT_MS = 30000;
  inline const char* NVS_KEY_GAS_SECRET = "gas_secret";
  constexpr uint8_t GAS_SECRET_LEN = 32;
  constexpr uint8_t GAS_HMAC_LEN = 32;
  constexpr uint16_t GAS_MAX_BODY_SIZE = 16384;
  constexpr int32_t GAS_TIMESTAMP_TOLERANCE_SEC = 300;

  // ---------- PZEM (referenced in real Config.h, not used by MqttClient) ----------
  constexpr uint8_t PZEM_RX_PIN = 5;
  constexpr uint8_t PZEM_TX_PIN = 4;
  constexpr uint32_t PZEM_BAUD_RATE = 9600;
  constexpr uint8_t PZEM_MODBUS_ADDR = 0x01;
  constexpr uint16_t PZEM_READ_INTERVAL_MS = 1000;
  constexpr uint16_t PZEM_TIMEOUT_MS = 1000;
  constexpr float VOLTAGE_MAINS_V = 220.0f;
  constexpr float ALARM_VOLTAGE_MIN = 190.0f;
  constexpr float ALARM_VOLTAGE_MAX = 250.0f;
  constexpr float ALARM_CURRENT_MAX = 8.0f;
  constexpr float ALARM_POWER_MAX = 1500.0f;
  constexpr float ALARM_PF_MIN = 0.70f;
  constexpr uint16_t ALARM_COOLDOWN_MS = 60000;

  // ---------- STORAGE LIMITS (more) ----------
  constexpr size_t MAX_BODY_SIZE = 16384;
  constexpr size_t MAX_AUDIT_LOG_SIZE = 8192;
  constexpr size_t MAX_ACTIVITY_LOG_ENTRIES = 200;
  constexpr unsigned long SAVE_DELAY_MS = 10000;
  constexpr unsigned long MAX_SAVE_DELAY_MS = 60000;
  constexpr uint32_t PBKDF2_ITERATIONS = 50000;

  // ---------- OTA ----------
  constexpr size_t OTA_MAX_BINARY_SIZE = 2 * 1024 * 1024;
  constexpr uint8_t ED25519_PUBLIC_KEY_LEN = 32;
  constexpr uint8_t ED25519_SIGNATURE_LEN = 64;
  constexpr size_t SHA256_HEX_LEN = 64;
  // Mutable: tests can set this to non-empty to exercise download path
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
  inline const char* NVS_KEY_JWT_SECRET = "jwt_secret";
  inline const char* JWT_SECRET_DEFAULT = "";

  inline const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
  inline const char* NVS_KEY_WIFI_PASS = "wifi_pass";
  inline const char* NVS_KEY_MQTT_PASS = "mqtt_pass";
  inline const char* NVS_KEY_DEVICE_PIN = "device_pin";

  // ---------- WIFI (referenced in real Config.h, not used by MqttClient) ----------
  inline const char* AP_SSID = "Timer12CH";
  constexpr uint8_t WIFI_CHANNEL = 6;
  constexpr uint8_t WIFI_MAX_CLIENTS = 4;
  constexpr bool WIFI_HIDDEN = false;
  constexpr int8_t WIFI_TX_POWER_DBM = 17;
  constexpr uint32_t AP_IP[] = {192, 168, 4, 1};
  inline const char* WIFI_CONFIG_PORTAL_SSID = "Timer12-Setup";
  inline const char* WIFI_CONFIG_PORTAL_PASSWORD = "";
  constexpr uint32_t WIFI_STA_TIMEOUT_MS = 15000;
  constexpr uint8_t WIFI_STA_MAX_RETRIES = 3;
  constexpr uint8_t CONFIG_VERSION = 2;
  inline const char* DEVICE_MODEL = "ESP32-WROOM-32 Timer12 v4.0 TEST";
  constexpr uint8_t I2C_SDA = 32;
  constexpr uint8_t I2C_SCL = 33;
  constexpr uint32_t I2C_CLOCK = 400000;
}

// =============================================================================
// ESP32 system symbols
// =============================================================================

// delay() — Arduino function, no-op on host
inline void delay(unsigned long /*ms*/) {}

// ESP class — Arduino ESP32 framework
class EspClass {
public:
  uint32_t getFreeHeap() { return 100 * 1024; }
  void restart() {
    // Set flag instead of restarting — test verifies behavior
    extern bool g_espRestartCalled;
    g_espRestartCalled = true;
  }
};
static inline EspClass ESP;

// esp_task_wdt — ESP-IDF watchdog
extern "C" {
  inline void esp_task_wdt_reset() {}
  inline void esp_task_wdt_init(uint32_t, bool) {}
  inline void esp_task_wdt_add(void*) {}
  inline void esp_task_wdt_delete(void*) {}
}

// arduino_event types (used by WifiManager.h's onEvent signature)
typedef int arduino_event_id_t;
typedef int arduino_event_info_t;

// =============================================================================
// mbedtls/sha256.h shim (real SHA-256 from OpenSSL — libcrypto)
// =============================================================================
typedef struct mbedtls_sha256_context {
  SHA256_CTX ctx;
} mbedtls_sha256_context;

inline void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
  if (ctx) SHA256_Init(&ctx->ctx);
}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int /*is224*/) {
  return (ctx && SHA256_Init(&ctx->ctx) == 1) ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx,
                                  const uint8_t* data, size_t len) {
  return (ctx && SHA256_Update(&ctx->ctx, data, len) == 1) ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t* out) {
  return (ctx && SHA256_Final(out, &ctx->ctx) == 1) ? 0 : -1;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context* /*ctx*/) {}

// =============================================================================
// WiFi / network shims
// =============================================================================

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

// =============================================================================
// PubSubClient stub — minimal MQTT client API used by MqttClient.cpp
// =============================================================================
// Test-controllable flags (definitions in MqttClientTest.cpp):
//   g_pubsubConnected       — connected() returns this
//   g_pubsubPublishSucceeds — publish() returns this
//   g_lastPublishedPayload  — captures the last published payload (for asserts)
//   g_lastPublishedTopic    — captures the topic of the last publish
//   g_publishCallCount      — counts publish() calls
// =============================================================================
extern bool g_pubsubConnected;
extern bool g_pubsubPublishSucceeds;
extern std::string g_lastPublishedPayload;
extern std::string g_lastPublishedTopic;
extern int g_publishCallCount;

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

  // Connection state — test-controllable
  bool connected() { return g_pubsubConnected; }

  bool connect(const char* /*clientId*/,
               const char* /*user*/, const char* /*pass*/,
               const char* /*willTopic*/, uint8_t /*willQos*/,
               bool /*willRetain*/, const char* /*willMsg*/) {
    return g_pubsubConnected;
  }

  bool subscribe(const char* /*topic*/, uint8_t /*qos*/) { return true; }

  bool publish(const char* topic, const char* payload, bool /*retain*/) {
    g_publishCallCount++;
    g_lastPublishedTopic = topic ? topic : "";
    g_lastPublishedPayload = payload ? payload : "";
    return g_pubsubPublishSucceeds;
  }
  bool publish(const char* topic, const uint8_t* payload, unsigned int len, bool /*retain*/) {
    g_publishCallCount++;
    g_lastPublishedTopic = topic ? topic : "";
    g_lastPublishedPayload.assign(reinterpret_cast<const char*>(payload), len);
    return g_pubsubPublishSucceeds;
  }

  void loop() {}
  int state() { return 0; }
};

// =============================================================================
// HTTPClient stub
// =============================================================================
#define HTTPC_STRICT_FOLLOW_REDIRECTS 1
#define HTTP_CODE_OK 200

class HTTPClient {
public:
  HTTPClient() {}
  void setFollowRedirects(int) {}
  void setTimeout(uint16_t) {}
  void setConnectTimeout(uint16_t) {}
  bool begin(WiFiClientSecure&, const String&) { return false; }
  bool begin(WiFiClientSecure&, const std::string&) { return false; }
  int sendRequest(const char*) { return 0; }
  int GET() { return 0; }
  int getSize() { return 0; }
  WiFiClient* getStreamPtr() { return nullptr; }
  void end() {}
};

// =============================================================================
// Update stub — OTA flash API
// =============================================================================
// Default behavior: begin() returns false (OTA cannot start). This causes
// _downloadAndVerifyOta to fail at the Update.begin() step, which is BEFORE
// the mbedtls/Ed25519 calls. Tests that exercise the OTA download path will
// hit this failure (or the earlier OTA_HTTPS_ROOT_CA empty check).
class UpdateClass {
public:
  bool begin(size_t /*size*/) { return false; }
  size_t write(uint8_t* /*buf*/, size_t len) { return len; }
  bool end(bool /*evenIfRemaining*/ = false) { return false; }
  bool isFinished() { return false; }
  void abort() {}
  uint8_t getError() { return 0; }
};
static inline UpdateClass Update;

// =============================================================================
// RTClib stub (RtcDriver.h includes <RTClib.h>)
// =============================================================================
class RTC_DS3231 {
public:
  bool begin() { return true; }
  bool now() { return true; }
};

// =============================================================================
// WebServer stub (AuthManager.h includes <WebServer.h>)
// =============================================================================
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

#endif // HOST_SHIM_MQTTCLIENT_SHIMS_H
