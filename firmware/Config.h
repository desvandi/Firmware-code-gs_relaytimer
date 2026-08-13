// =============================================================================
// Core/Config.h — System-wide constants and configuration
// Timer Digital Relay v4.0 — Cloud-Ready Architecture
// =============================================================================
#pragma once
#ifndef TIMER12_CORE_CONFIG_H
#define TIMER12_CORE_CONFIG_H

#include <Arduino.h>
#include <cstdint>

// ---------- FIRMWARE VERSION ----------
namespace Core {
  constexpr char FIRMWARE_VERSION[] = "4.0.0";
  constexpr char BUILD_DATE[] = __DATE__ " " __TIME__;
  constexpr uint8_t CONFIG_VERSION = 2;  // bump when schedule.json schema changes
  constexpr char DEVICE_MODEL[] = "ESP32-WROOM-32 Timer12 v4.0";

  // ---------- CHANNELS ----------
  constexpr uint8_t NUM_CHANNELS = 12;
  constexpr uint8_t NUM_PIR = 4;
  constexpr uint8_t PIR_CHANNEL_OFFSET = 8;  // PIR 1..4 -> channel 9..12 (index 8..11)
  constexpr uint8_t MAX_SCHEDULES = 4;

  // ---------- PIN MAPPING ----------
  // Relay outputs (active-LOW module: LOW = ON, HIGH = OFF)
  constexpr uint8_t RELAY_PINS[NUM_CHANNELS] = {
    13, 14, 16, 17, 18, 19,
    21, 22, 23, 25, 26, 27
  };
  constexpr uint8_t RELAY_ON = LOW;
  constexpr uint8_t RELAY_OFF = HIGH;

  // PIR inputs (input-only GPIO, no internal pull needed for HC-SR501)
  constexpr uint8_t PIR_PINS[NUM_PIR] = {34, 35, 36, 39};

  // I2C for DS3231 RTC
  constexpr uint8_t I2C_SDA = 32;
  constexpr uint8_t I2C_SCL = 33;
  constexpr uint32_t I2C_CLOCK = 400000;  // Fast Mode

  // ---------- WIFI AP (fallback mode) ----------
  constexpr const char* AP_SSID = "Timer12CH";
  constexpr uint8_t WIFI_CHANNEL = 6;
  constexpr uint8_t WIFI_MAX_CLIENTS = 4;
  constexpr bool WIFI_HIDDEN = false;
  constexpr int8_t WIFI_TX_POWER_DBM = 17;
  constexpr uint32_t AP_IP[] = {192, 168, 4, 1};

  // ---------- WIFI STA (primary mode — join MiFi/router) ----------
  // WiFi credentials are stored in NVS (Preferences) via WiFi Config Portal.
  // On first boot: ESP32 starts AP "Timer12-Setup", user enters SSID+password via web form.
  // After save: ESP32 reboots to STA mode and joins the configured WiFi.
  // If STA connection fails after 3 retries: Config Portal reopens automatically.
  constexpr const char* WIFI_CONFIG_PORTAL_SSID = "Timer12-Setup";
  constexpr const char* WIFI_CONFIG_PORTAL_PASSWORD = "";  // empty = open AP (easy onboarding)
  constexpr uint32_t WIFI_STA_TIMEOUT_MS = 15000;
  constexpr uint8_t WIFI_STA_MAX_RETRIES = 3;
  // NVS keys
  constexpr const char* NVS_NAMESPACE = "timer12";
  constexpr const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
  constexpr const char* NVS_KEY_WIFI_PASS = "wifi_pass";
  constexpr const char* NVS_KEY_MQTT_PASS = "mqtt_pass";  // topic password for security
  constexpr const char* NVS_KEY_DEVICE_PIN = "device_pin";  // 6-digit PIN for PWA pairing

  // ---------- MQTT BROKER (remote internet access via CGNAT) ----------
  // PRODUCTION: Self-hosted Mosquitto with TLS + ACL + per-device credentials.
  //   1. Deploy Mosquitto on VPS (DigitalOcean/Hetzner ~Rp 75rb/bln)
  //   2. Config: allow_anonymous false, password_file, ACL per device, listener 8883
  //   3. TLS: Let's Encrypt cert for your domain (or self-signed with bundled CA)
  //   4. Set MQTT_BROKER_HOST, MQTT_BROKER_PORT=8883, MQTT_BROKER_USERNAME, MQTT_BROKER_PASSWORD
  //   5. Set MQTT_ROOT_CA to your CA certificate (PEM format, as multi-line string literal)
  //   6. Re-flash firmware, update PWA NEXT_PUBLIC_MQTT_BROKER_URL
  //
  // For development/MVP: leave MQTT_BROKER_USERNAME empty for public broker (no auth).
  // Topic password still provides basic obscurity but is NOT authentication.
  constexpr const char* MQTT_BROKER_HOST = "broker.hivemq.com";
  constexpr uint16_t MQTT_BROKER_PORT = 1883;
  constexpr const char* MQTT_BROKER_USERNAME = "";  // Empty = no auth (public broker). PRODUCTION: set per-device credential.
  constexpr const char* MQTT_BROKER_PASSWORD = "";  // Empty = no auth. PRODUCTION: set per-device credential.
  constexpr uint16_t MQTT_KEEPALIVE_SEC = 60;
  constexpr uint16_t MQTT_RECONNECT_DELAY_MS = 5000;
  constexpr uint16_t MQTT_STATUS_PUBLISH_INTERVAL_MS = 5000;
  constexpr uint16_t MQTT_BUFFER_SIZE = 4096;
  constexpr uint8_t MQTT_PASSWORD_LEN = 8;  // 8-char alphanumeric topic password

  // TLS root CA for MQTT broker (PEM format, multi-line string).
  // Default: empty → uses setInsecure() (NOT for production!).
  // For Let's Encrypt signed broker (recommended): paste ISRG Root X1 PEM here.
  // For self-signed broker: paste your custom CA PEM here.
  // Get ISRG Root X1 from: https://letsencrypt.org/certs/isrgrootx1.pem
  constexpr const char* MQTT_ROOT_CA = "";

  // ---------- CORS (Cross-Origin Resource Sharing) ----------
  // Controls which web origins can call the ESP32 REST API.
  // Default: "*" (any origin — for development only).
  // PRODUCTION: set to your PWA's Vercel URL, e.g., "https://remote-relay.vercel.app".
  // Multiple origins: comma-separated, first match wins.
  constexpr const char* ALLOWED_CORS_ORIGINS = "*";

  // ---------- GOOGLE APPS SCRIPT (AI Insights via Gemini) ----------
  // Deploy Code.gs as a GAS Web App, then paste the deployment URL here.
  // ESP32 POSTs logs + status to this URL every hour for AI analysis.
  // Leave empty ("") to disable AI insights — PWA will show mock cards.
  // Format: https://script.google.com/macros/s/AKfyc.../exec
  constexpr const char* GAS_INSIGHTS_URL = "https://script.google.com/macros/s/AKfycbwAAQYaLZhE7RVktWi_GElKeZS49JYLVXGdlw7bpqDEViAA-pstUtul6yU5T1rH9OPlug/exec";
  constexpr uint32_t GAS_POST_INTERVAL_MS = 3600000;  // 1 hour
  constexpr uint16_t GAS_TIMEOUT_MS = 30000;  // 30s timeout

  // GAS HMAC shared secret (P0 #7 — audit round 9).
  // ESP32 generates a 32-byte random secret at first boot, stores in NVS,
  // prints to Serial. User copies to GAS Script Properties as:
  //   DEVICE_<anonymousId>_SECRET = <hex secret>
  // ESP32 signs each POST with HMAC-SHA256(secret, canonical_request).
  // GAS verifies signature before processing.
  constexpr const char* NVS_KEY_GAS_SECRET = "gas_secret";
  constexpr uint8_t GAS_SECRET_LEN = 32;       // 256-bit
  constexpr uint8_t GAS_HMAC_LEN = 32;         // SHA-256 output bytes
  constexpr uint16_t GAS_MAX_BODY_SIZE = 16384; // 16 KB cap
  constexpr int32_t GAS_TIMESTAMP_TOLERANCE_SEC = 300;  // ±5 minutes

  // ---------- PZEM-004T v3.0 POWER METER (AC 80-260V) ----------
  // Measures: voltage, current, power, energy (kWh), frequency, power factor
  // Communication: Modbus-RTU over UART (5V TTL, 9600 baud)
  // Library: self-contained (no external dependency — implements Modbus directly)
  //
  // Wiring:
  //   PZEM VCC → 5V
  //   PZEM GND → GND (shared with ESP32)
  //   PZEM TX  → GPIO5 (ESP32 RX via UART1)
  //   PZEM RX  → GPIO4 (ESP32 TX via UART1)
  //   Note: PZEM TX is 5V. ESP32 GPIO5 is 3.3V input but 5V-tolerant on digital reads.
  //   For long-term safety, add a 1K resistor in series between PZEM TX and ESP32 RX.
  //
  // PZEM-004T v3.0 specs:
  //   Voltage: 80-260V AC, ±0.5% accuracy
  //   Current: 0-10A (with built-in CT), ±0.5% accuracy
  //   Power: 0-2.3kW, ±1.0% accuracy
  //   Energy: 0-9999.99 kWh, ±1.0% accuracy
  //   Frequency: 45-65 Hz
  //   Power Factor: 0-1.0
  constexpr uint8_t PZEM_RX_PIN = 5;           // ESP32 RX ← PZEM TX
  constexpr uint8_t PZEM_TX_PIN = 4;           // ESP32 TX → PZEM RX
  constexpr uint32_t PZEM_BAUD_RATE = 9600;
  constexpr uint8_t PZEM_MODBUS_ADDR = 0x01;   // Default Modbus address
  constexpr uint16_t PZEM_READ_INTERVAL_MS = 1000;  // Read every 1 second
  constexpr uint16_t PZEM_TIMEOUT_MS = 1000;   // Response timeout
  constexpr float VOLTAGE_MAINS_V = 220.0;     // Reference mains voltage (Indonesia)

  // ---------- POWER ALARM THRESHOLDS ----------
  // ESP32 checks PZEM readings against these thresholds every read cycle.
  // If exceeded: publishes alarm via MQTT + logs to activity log.
  // All configurable via PWA (Settings) in future; hardcoded for MVP.
  constexpr float ALARM_VOLTAGE_MIN = 190.0;     // V — undervoltage
  constexpr float ALARM_VOLTAGE_MAX = 250.0;     // V — overvoltage
  constexpr float ALARM_CURRENT_MAX = 8.0;       // A — overcurrent (PZEM max 10A)
  constexpr float ALARM_POWER_MAX = 1500.0;      // W — overpower
  constexpr float ALARM_PF_MIN = 0.70;           // Power factor — low PF (inductive load)
  constexpr uint16_t ALARM_COOLDOWN_MS = 60000;  // Don't re-trigger same alarm for 60s

  // ---------- STORAGE LIMITS ----------
  constexpr size_t MAX_NAME_LEN = 20;
  constexpr size_t MAX_NAME_BUF = MAX_NAME_LEN + 1;
  constexpr size_t MAX_USER_LEN = 31;
  constexpr size_t MAX_USER_BUF = MAX_USER_LEN + 1;
  constexpr size_t MAX_TIME_STR = 5;       // "HH:MM"
  constexpr size_t MAX_TIME_BUF = 6;       // +null
  constexpr size_t SALT_LEN = 16;
  constexpr size_t HASH_LEN = 32;          // SHA-256 output bytes
  constexpr size_t HASH_HEX_LEN = HASH_LEN * 2;
  constexpr size_t HASH_HEX_BUF_SIZE = HASH_HEX_LEN + 1;
  constexpr uint16_t PBKDF2_ITERATIONS = 10000;

  constexpr size_t MAX_BODY_SIZE = 16384;       // 16 KB for /api/config/import
  constexpr size_t MAX_AUDIT_LOG_SIZE = 8192;   // 8 KB before rotation
  constexpr size_t MAX_ACTIVITY_LOG_ENTRIES = 200;

  // ---------- TIMING ----------
  constexpr unsigned long SAVE_DELAY_MS = 10000;       // 10s debounce
  constexpr unsigned long MAX_SAVE_DELAY_MS = 60000;   // 60s forced save
  constexpr unsigned long PIR_WARMUP_MS = 60000;       // 60s warmup after boot
  constexpr unsigned long PIR_DEBOUNCE_INTERVAL_MS = 50;
  constexpr uint8_t PIR_DEBOUNCE_SAMPLES = 3;
  constexpr uint8_t PIR_DEBOUNCE_THRESHOLD = 2;       // 2 of 3 = HIGH
  constexpr unsigned long PIR_STUCK_TIMEOUT_MS = 1800000;     // 30 min
  constexpr unsigned long PIR_STUCK_COOLDOWN_MS = 300000;     // 5 min

  constexpr unsigned long RELAY_TICK_MS = 1000;        // recompute relay state every 1s

  // ---------- AUTH ----------
  // P1 #17 (audit round 9): Short-lived access token + long-lived refresh token.
  // Access token TTL: 15 min (stateless JWT, verified by signature only).
  // Refresh token TTL: 7 days (stored in NVS, one-time use per login).
  // On access token expiry, PWA calls POST /api/refresh with refresh token cookie
  // → ESP32 issues new access token (and rotates refresh token).
  constexpr uint8_t AUTH_FAIL_THRESHOLD_SHORT = 5;
  constexpr uint8_t AUTH_FAIL_THRESHOLD_LONG = 10;
  constexpr unsigned long AUTH_BLOCK_SHORT_MS = 60000;     // 1 min
  constexpr unsigned long AUTH_BLOCK_LONG_MS = 300000;     // 5 min
  constexpr size_t MAX_TRACKED_IPS = 8;
  constexpr uint16_t CSRF_TOKEN_LEN = 32;                  // hex chars (16 bytes random)
  constexpr unsigned long CSRF_TOKEN_TTL_MS = 900000;      // 15 min (matches access token)
  constexpr uint16_t JWT_ACCESS_TTL_SECONDS = 900;         // 15 min (access token)
  constexpr uint16_t JWT_REFRESH_TTL_SECONDS = 604800;     // 7 days (refresh token)
  constexpr uint16_t JWT_TTL_SECONDS = JWT_ACCESS_TTL_SECONDS;  // alias for legacy code
  constexpr size_t JWT_MAX_LEN = 512;
  constexpr size_t REFRESH_TOKEN_LEN = 32;                 // hex chars (16 bytes random)
  constexpr uint8_t MAX_REFRESH_TOKENS = 4;                // cap per device (LRU)

  // ---------- FACTORY RESET ----------
  constexpr unsigned long FACTORY_RESET_TOKEN_TTL_MS = 60000;  // 60s

  // ---------- OTA (P0 #3+#8+#9 — audit round 9) ----------
  // MQTT OTA now requires Ed25519 signature verification.
  // PWA must send: {action, url, version, size, sha256, signature, requestId}
  // ESP32: HTTPS download → size check → SHA-256 verify → Ed25519 verify → Update.
  //
  // Signing keypair generation:
  //   openssl genpkey -algorithm Ed25519 -out firmware_signing_private.pem
  //   openssl pkey -in firmware_signing_private.pem -pubout -out firmware_signing_public.pem
  //   # Sign firmware.bin:
  //   openssl pkeyutl -sign -inkey firmware_signing_private.pem -rawin -in firmware.bin | xxd -p -c 64 > firmware.bin.sig
  //
  // Embed the PUBLIC key below (32 bytes hex, no leading "0x").
  // Private key stays on signing machine — NEVER in firmware.
  constexpr size_t OTA_MAX_BINARY_SIZE = 2 * 1024 * 1024;  // 2 MB safety cap
  constexpr uint8_t ED25519_PUBLIC_KEY_LEN = 32;           // raw public key bytes
  constexpr uint8_t ED25519_SIGNATURE_LEN = 64;            // raw signature bytes
  constexpr size_t SHA256_HEX_LEN = 64;                    // hex-encoded SHA-256
  // PRODUCTION: paste your Ed25519 public key (32 bytes as 64 hex chars) here.
  // Empty = signature verification skipped (NOT for production!).
  constexpr const char* OTA_ED25519_PUBLIC_KEY_HEX = "";

  // Root CA for HTTPS OTA downloads (GitHub Releases, etc.)
  // Default: empty → uses WiFiClient (plain HTTP, NOT for production!).
  // For GitHub: use DigiCert/GlobalSign root CA.
  constexpr const char* OTA_HTTPS_ROOT_CA = "";

  // ---------- FILE PATHS ----------
  constexpr const char* PATH_CONFIG_JSON = "/config.json";
  constexpr const char* PATH_CONFIG_BAK = "/config.bak";
  constexpr const char* PATH_CONFIG_TMP = "/config.tmp";
  constexpr const char* PATH_SCHEDULE_JSON = "/schedule.json";
  constexpr const char* PATH_SCHEDULE_BAK = "/schedule.bak";
  constexpr const char* PATH_SCHEDULE_TMP = "/schedule.tmp";
  constexpr const char* PATH_SCHEDULE_BAK_TMP = "/schedule.bak.tmp";
  constexpr const char* PATH_AUDIT_LOG = "/audit.log";
  constexpr const char* PATH_AUDIT_LOG_OLD = "/audit.log.old";
  constexpr const char* PATH_ACTIVITY_LOG = "/activity.log";
  constexpr const char* PATH_ACTIVITY_LOG_OLD = "/activity.log.old";

  // ---------- DEFAULT TIMEZONE ----------
  constexpr const char* DEFAULT_TIMEZONE = "Asia/Jakarta";

  // ---------- JWT (mock secret — burn into NVS in production) ----------
  // In production: store a per-device random 32-byte secret in Preferences/NVS
  // at first boot. Here we use a compile-time constant for demonstration.
  constexpr const char* JWT_SECRET_DEFAULT = "Timer12-v4.0-CHANGE-ME-IN-PRODUCTION";
}

#endif // TIMER12_CORE_CONFIG_H
