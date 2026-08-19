// =============================================================================
// ErrorCodes.h — Centralized error code registry (brief §59)
// Timer Digital Relay v4.2 — Industrial-Grade Hardening
// -----------------------------------------------------------------------------
// All error codes are deterministic strings. PWA can match on these to render
// localized error messages. NEVER use ad-hoc error messages.
//
// Format: ERR_<DOMAIN>_<NNN>
// =============================================================================
#pragma once
#ifndef TIMER12_ERROR_CODES_H
#define TIMER12_ERROR_CODES_H

namespace Services { namespace Err {

// ---------- AUTH (brief §32) ----------
constexpr const char* AUTH_INVALID_CREDENTIALS   = "ERR_AUTH_001";
constexpr const char* AUTH_EXPIRED_TOKEN          = "ERR_AUTH_002";
constexpr const char* AUTH_MISSING_TOKEN          = "ERR_AUTH_003";
constexpr const char* AUTH_INVALID_SIGNATURE     = "ERR_AUTH_004";
constexpr const char* AUTH_RATE_LIMITED           = "ERR_AUTH_005";
constexpr const char* AUTH_BLOCKED                = "ERR_AUTH_006";
constexpr const char* AUTH_CSRF_INVALID           = "ERR_AUTH_007";
constexpr const char* AUTH_REFRESH_INVALID        = "ERR_AUTH_008";

// ---------- COMMAND (brief §24-28) ----------
constexpr const char* CMD_INVALID_REQUEST_ID     = "ERR_CMD_001";
constexpr const char* CMD_REPLAYED                = "ERR_CMD_002";
constexpr const char* CMD_REQUEST_ID_COLLISION   = "ERR_CMD_003";
constexpr const char* CMD_SEQUENCE_REORDERED     = "ERR_CMD_004";
constexpr const char* CMD_UNKNOWN_ACTION          = "ERR_CMD_005";
constexpr const char* CMD_INVALID_FIELD           = "ERR_CMD_006";
constexpr const char* CMD_OVERSIZED                = "ERR_CMD_007";
constexpr const char* CMD_REJECTED_INTERLOCK     = "ERR_CMD_008";

// ---------- RELAY (brief §10-16) ----------
constexpr const char* RELAY_CHANNEL_INVALID      = "ERR_RELAY_001";
constexpr const char* RELAY_MAX_ON_TIME         = "ERR_RELAY_002";  // §14: force-off
constexpr const char* RELAY_MIN_ON_TIME         = "ERR_RELAY_003";  // §15: inhibit OFF
constexpr const char* RELAY_MIN_OFF_TIME        = "ERR_RELAY_004";  // §15: inhibit ON
constexpr const char* RELAY_ANTI_CHATTER         = "ERR_RELAY_005";  // §16: too rapid
constexpr const char* RELAY_INTERLOCK_VIOLATION = "ERR_RELAY_006";  // §9
constexpr const char* RELAY_STATE_DRIFT          = "ERR_RELAY_007";  // §11
constexpr const char* RELAY_BOOT_POLICY_FAIL     = "ERR_RELAY_008";  // §13

// ---------- SENSOR (brief §20-21) ----------
constexpr const char* SENSOR_I2C_ERROR           = "ERR_SENSOR_001";
constexpr const char* SENSOR_CRC_ERROR           = "ERR_SENSOR_002";
constexpr const char* SENSOR_OUT_OF_RANGE        = "ERR_SENSOR_003";
constexpr const char* SENSOR_STALE                = "ERR_SENSOR_004";
constexpr const char* SENSOR_UNAVAILABLE         = "ERR_SENSOR_005";
constexpr const char* SENSOR_PLAUSIBILITY_FAIL  = "ERR_SENSOR_006";  // §21

// ---------- RTC (brief §18-19) ----------
constexpr const char* RTC_INVALID                 = "ERR_RTC_001";   // §18
constexpr const char* RTC_UNSYNCED                = "ERR_RTC_002";
constexpr const char* RTC_DRIFT_WARNING           = "ERR_RTC_003";   // §19
constexpr const char* RTC_SCHEDULER_INHIBITED     = "ERR_RTC_004";   // §19 critical

// ---------- STORAGE (brief §30-31) ----------
constexpr const char* STORAGE_NVS_WRITE_FAIL     = "ERR_STORAGE_001";
constexpr const char* STORAGE_NVS_READ_FAIL     = "ERR_STORAGE_002";
constexpr const char* STORAGE_CONFIG_CRC_FAIL   = "ERR_STORAGE_003";
constexpr const char* STORAGE_CONFIG_VERSION_MISMATCH = "ERR_STORAGE_004";
constexpr const char* STORAGE_JOURNAL_FULL       = "ERR_STORAGE_005";
constexpr const char* STORAGE_JOURNAL_CRC_FAIL   = "ERR_STORAGE_006";

// ---------- OTA (brief §49) ----------
constexpr const char* OTA_DOWNLOAD_FAIL          = "ERR_OTA_001";
constexpr const char* OTA_SIZE_EXCEEDED          = "ERR_OTA_002";
constexpr const char* OTA_SHA256_MISMATCH       = "ERR_OTA_003";
constexpr const char* OTA_ED25519_INVALID        = "ERR_OTA_004";
constexpr const char* OTA_DOWNGRADE_BLOCKED     = "ERR_OTA_005";  // anti-downgrade
constexpr const char* OTA_URL_NOT_ALLOWLISTED    = "ERR_OTA_006";

// ---------- NETWORK (brief §78) ----------
constexpr const char* NET_WIFI_CONNECT_FAIL      = "ERR_NET_001";
constexpr const char* NET_MQTT_CONNECT_FAIL     = "ERR_NET_002";
constexpr const char* NET_MQTT_PUBLISH_FAIL     = "ERR_NET_003";
constexpr const char* NET_GAS_UNREACHABLE        = "ERR_NET_004";
constexpr const char* NET_DNS_FAIL               = "ERR_NET_005";

// ---------- CONFIG (brief §30-31, §96) ----------
constexpr const char* CONFIG_VALIDATION_FAIL     = "ERR_CONFIG_001";
constexpr const char* CONFIG_MIGRATION_FAIL     = "ERR_CONFIG_002";
constexpr const char* CONFIG_A_B_COMMIT_FAIL   = "ERR_CONFIG_003";

// ---------- SECURITY (brief §51, §88) ----------
constexpr const char* SEC_REPLAY_DETECTED         = "ERR_SEC_001";
constexpr const char* SEC_NONCE_REUSE              = "ERR_SEC_002";
constexpr const char* SEC_HMAC_MISMATCH            = "ERR_SEC_003";
constexpr const char* SEC_CROSS_DEVICE_ACCESS     = "ERR_SEC_004";
constexpr const char* SEC_PRIVILEGE_ESCALATION    = "ERR_SEC_005";
constexpr const char* SEC_FACTORY_RESET_BLOCKED   = "ERR_SEC_006";

}} // namespace Services::Err

#endif // TIMER12_ERROR_CODES_H
