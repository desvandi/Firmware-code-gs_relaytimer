// =============================================================================
// Services/CommandCanonicalizer.cpp — PD-001 Canonical Command implementation
// =============================================================================
// This is the SINGLE source of truth for canonical command serialization and
// hashing. Both REST (RelayHandlers.h, ScheduleHandlers.h, ...) and MQTT
// (MqttClient.cpp) call into this module — they MUST NOT compute their own
// hash (per directive §18 / AC-001 / AC-018).
//
// The schema below is a 1:1 extraction of the per-type schema that previously
// lived as a static helper inside MqttClient.cpp. Moving it here does NOT
// change MQTT behavior (regression-safe) but DOES enable REST to share it.
// =============================================================================
#include "CommandCanonicalizer.h"
#include "TransactionJournal.h"
#include "LogService.h"

namespace Services {

// ---------------------------------------------------------------------------
// validateTransactionId
// ---------------------------------------------------------------------------
bool CommandCanonicalizer::validateTransactionId(const String& tid,
                                                 String& errorMessageOut) {
  if (tid.length() < MIN_TRANSACTION_ID_LEN) {
    errorMessageOut = "transactionId is required (non-empty)";
    return false;
  }
  if (tid.length() > MAX_TRANSACTION_ID_LEN) {
    errorMessageOut = "transactionId too long (max " +
                      String(MAX_TRANSACTION_ID_LEN) + " chars)";
    return false;
  }
  // Allowed charset: [a-zA-Z0-9-_]  (no control chars, no whitespace)
  // Per directive §5: case handling must be consistent and documented.
  // We ACCEPT mixed case but do not normalize — caller responsibility.
  for (size_t i = 0; i < tid.length(); i++) {
    char c = tid[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      errorMessageOut = "transactionId contains invalid char at pos " +
                        String(i) + " (allowed: a-z A-Z 0-9 - _)";
      return false;
    }
  }
  errorMessageOut = "";
  return true;
}

// ---------------------------------------------------------------------------
// validateProtocolVersion
// ---------------------------------------------------------------------------
bool CommandCanonicalizer::validateProtocolVersion(int version,
                                                   String& errorMessageOut) {
  if (version == (int)CANONICAL_COMMAND_VERSION) {
    errorMessageOut = "";
    return true;
  }
  // Per directive §13: NO silent downgrade. Unsupported → REJECT.
  errorMessageOut = "unsupported protocol version " + String(version) +
                    " (supported: " + String(CANONICAL_COMMAND_VERSION) + ")";
  return false;
}

// ---------------------------------------------------------------------------
// isKnownCommandType — registry of (type, action) pairs.
//
// This registry MUST match the dispatch tables in:
//   - MqttClient.cpp::_handleCommand()  (the `else if (strcmp(type,...))` chain)
//   - MqttClient.cpp::_handleOta()      (type="ota")
//   - REST handler route registrations in HttpServer.cpp
//
// Per directive §15: this is a minimal registry abstraction, not a framework.
// ---------------------------------------------------------------------------
bool CommandCanonicalizer::isKnownCommandType(const String& type,
                                              const String& action) {
  // type and action are expected to be already lowercased by caller.
  if (type == "relay") {
    return action == "on" || action == "off" || action == "set_mode";
  }
  if (type == "schedule") {
    return action == "upsert" || action == "delete";
  }
  if (type == "pir") {
    return action == "config" || action == "test";
  }
  if (type == "channel") {
    return action == "rename";
  }
  if (type == "time") {
    return action == "set";
  }
  if (type == "system") {
    return action == "reboot" || action == "getStatus" ||
           action == "resetEnergyStats" || action == "resetDailyStats";
  }
  if (type == "config") {
    return action == "setDevice";
  }
  if (type == "ota") {
    // OTA command shape is defined in MqttClient.cpp::_handleOta().
    // Hashing of OTA commands is supported (same schema as MQTT) but REST
    // OTA upload uses multipart and is NOT routed through this module.
    return action == "update";  // MQTT OTA action (per _handleOta)
  }
  return false;
}

// ---------------------------------------------------------------------------
// isFieldAllowed — per-type allowed payload field whitelist.
//
// Must match the unknown-field rejection logic in MqttClient.cpp::_handleCommand
// (R10D-3 block, lines ~768..823 in the original source).
// ---------------------------------------------------------------------------
bool CommandCanonicalizer::isFieldAllowed(const String& type,
                                          const String& fieldName) {
  // Envelope fields — always allowed.
  if (fieldName == "type" || fieldName == "action" ||
      fieldName == "requestId" || fieldName == "transactionId" ||
      fieldName == "version" || fieldName == "issuedAt" ||
      fieldName == "expiresAt") {
    return true;
  }
  // Per-type payload field whitelist.
  if (type == "relay") {
    return fieldName == "channelId" || fieldName == "mode" ||
           fieldName == "manualState";
  }
  if (type == "schedule") {
    return fieldName == "channelId" || fieldName == "id" ||
           fieldName == "onTime" || fieldName == "offTime" ||
           fieldName == "dayMask" || fieldName == "enabled";
  }
  if (type == "pir") {
    return fieldName == "id" || fieldName == "enabled" ||
           fieldName == "holdTime";
  }
  if (type == "channel") {
    return fieldName == "channelId" || fieldName == "name";
  }
  if (type == "time") {
    return fieldName == "datetime";
  }
  if (type == "system") {
    // system: action only — no payload fields.
    return false;
  }
  if (type == "config") {
    return fieldName == "deviceName" || fieldName == "timezone";
  }
  if (type == "ota") {
    return fieldName == "url" || fieldName == "version" ||
           fieldName == "size" || fieldName == "sha256" ||
           fieldName == "signature";
  }
  return false;
}

// ---------------------------------------------------------------------------
// buildCanonicalString — deterministic canonical serialization.
//
// Format: "v{version}|{type_lower}|{action_lower}|field1=val1|field2=val2|..."
//
// Field order is FIXED per type. Values are rendered explicitly:
//   - bool   → "true" / "false"
//   - int    → decimal string
//   - string → verbatim (NO lowercasing of payload strings)
//
// This function MUST produce byte-identical output for two semantically
// equivalent commands regardless of:
//   - JSON whitespace
//   - JSON property order
//   - transport (REST vs MQTT)
//   - requestId value
// ---------------------------------------------------------------------------
String CommandCanonicalizer::buildCanonicalString(const JsonDocument& doc,
                                                  const String& type,
                                                  const String& action,
                                                  String& errorMessageOut) {
  // Protocol version: default to 1 if not present, validate if present.
  int version = doc["version"] | (int)CANONICAL_COMMAND_VERSION;
  if (!validateProtocolVersion(version, errorMessageOut)) {
    return "";
  }

  // type and action: lowercase (per directive §8).
  // We do NOT use toLowerCase() on the payload — only on type/action.
  String typeLower = type;
  typeLower.toLowerCase();
  String actionLower = action;
  actionLower.toLowerCase();

  // Verify (type, action) is registered.
  // NOTE: We do NOT reject unknown (type, action) pairs here — only unknown types.
  // Action validation is the dispatcher's responsibility (preserves existing
  // MQTT behavior where action validation happens per-type in the dispatch
  // switch, AFTER the hash is computed). REST handlers may call
  // isKnownCommandType() explicitly for nicer error messages.
  // The type-level check below ensures we have a schema for field extraction.
  bool typeHasSchema = (typeLower == "relay" || typeLower == "schedule" ||
                        typeLower == "pir" || typeLower == "channel" ||
                        typeLower == "time" || typeLower == "system" ||
                        typeLower == "config" || typeLower == "ota");
  if (!typeHasSchema) {
    errorMessageOut = "unknown command type: " + typeLower;
    return "";
  }

  // Start canonical string.
  String canonical = "v" + String(version) + "|" + typeLower + "|" + actionLower;

  // Per-type payload field extraction in DETERMINISTIC ORDER.
  // Each field is rendered with explicit type representation.
  if (typeLower == "relay") {
    // Order: channelId, mode, manualState
    int channelId = doc["channelId"] | 0;
    const char* mode = doc["mode"] | "";
    bool manualState = doc["manualState"] | false;
    canonical += "|channelId=" + String(channelId);
    canonical += "|mode=" + String(mode);
    canonical += "|manualState=" + String(manualState ? "true" : "false");
  }
  else if (typeLower == "schedule") {
    // Order: channelId, id, onTime, offTime, dayMask, enabled
    int channelId = doc["channelId"] | 0;
    int id = doc["id"] | 0;
    const char* onTime = doc["onTime"] | "";
    const char* offTime = doc["offTime"] | "";
    int dayMask = doc["dayMask"] | 0;
    bool enabled = doc["enabled"] | true;
    canonical += "|channelId=" + String(channelId);
    canonical += "|id=" + String(id);
    canonical += "|onTime=" + String(onTime);
    canonical += "|offTime=" + String(offTime);
    canonical += "|dayMask=" + String(dayMask);
    canonical += "|enabled=" + String(enabled ? "true" : "false");
  }
  else if (typeLower == "pir") {
    // Order: id, enabled, holdTime
    int id = doc["id"] | 0;
    bool enabled = doc["enabled"] | false;
    int holdTime = doc["holdTime"] | 0;
    canonical += "|id=" + String(id);
    canonical += "|enabled=" + String(enabled ? "true" : "false");
    canonical += "|holdTime=" + String(holdTime);
  }
  else if (typeLower == "channel") {
    // Order: channelId, name
    int channelId = doc["channelId"] | 0;
    const char* name = doc["name"] | "";
    canonical += "|channelId=" + String(channelId);
    canonical += "|name=" + String(name);
  }
  else if (typeLower == "time") {
    const char* dt = doc["datetime"] | "";
    canonical += "|datetime=" + String(dt);
  }
  else if (typeLower == "system") {
    // system: action only (no payload fields hashed)
    // hash depends only on version+type+action.
  }
  else if (typeLower == "config") {
    // Order: deviceName, timezone
    const char* dn = doc["deviceName"] | "";
    const char* tz = doc["timezone"] | "";
    canonical += "|deviceName=" + String(dn);
    canonical += "|timezone=" + String(tz);
  }
  else if (typeLower == "ota") {
    // Order: url, version, size, sha256, signature
    const char* url = doc["url"] | "";
    const char* ver = doc["version"] | "";
    // Note: doc["version"] collision with protocol version field — OTA uses
    // "version" as firmware version string. We treat it as payload here.
    // The protocol envelope version is read above via doc["version"]|1 default,
    // but for OTA commands the caller should set protocol version explicitly.
    unsigned long size = (unsigned long)(doc["size"] | 0);
    const char* sha256 = doc["sha256"] | "";
    const char* signature = doc["signature"] | "";
    canonical += "|url=" + String(url);
    canonical += "|version=" + String(ver);
    canonical += "|size=" + String(size);
    canonical += "|sha256=" + String(sha256);
    canonical += "|signature=" + String(signature);
  }
  else {
    // Should not reach here — isKnownCommandType already filtered.
    errorMessageOut = "internal: unhandled type in buildCanonicalString: " + typeLower;
    return "";
  }

  errorMessageOut = "";
  return canonical;
}

// ---------------------------------------------------------------------------
// canonicalizeAndHash — top-level entry point used by both REST and MQTT.
// ---------------------------------------------------------------------------
CanonicalResult CommandCanonicalizer::canonicalizeAndHash(const JsonDocument& doc) {
  CanonicalResult result;
  result.ok = false;

  // Extract type and action.
  const char* typeC = doc["type"] | "";
  const char* actionC = doc["action"] | "";
  if (typeC[0] == '\0') {
    result.errorMessage = "missing 'type' field";
    return result;
  }
  if (actionC[0] == '\0') {
    result.errorMessage = "missing 'action' field";
    return result;
  }

  String type = String(typeC);
  String action = String(actionC);
  type.toLowerCase();
  action.toLowerCase();

  // Extract transactionId (requestId compatibility).
  // Per directive §4.1: requestId == transactionId (semantic alias).
  // We accept either field name; if both present, they MUST match.
  String tid = doc["requestId"] | "";
  String tidAlt = doc["transactionId"] | "";
  if (tid.length() == 0 && tidAlt.length() > 0) {
    tid = tidAlt;
  } else if (tid.length() > 0 && tidAlt.length() > 0 && tid != tidAlt) {
    // Two different identities for one mutation — forbidden per directive §4.1.
    result.errorMessage = "requestId and transactionId both present but differ";
    return result;
  }
  result.transactionId = tid;

  // Validate transactionId.
  String tidErr;
  if (!validateTransactionId(tid, tidErr)) {
    result.errorMessage = tidErr;
    return result;
  }

  // Build canonical string (includes version + type + action + payload).
  String canonErr;
  String canonical = buildCanonicalString(doc, type, action, canonErr);
  if (canonical.length() == 0) {
    result.errorMessage = canonErr;
    return result;
  }

  // Hash.
  String hash = Utils::sha256Hex(canonical);
  if (hash.length() == 0) {
    result.errorMessage = "SHA-256 computation failed";
    return result;
  }

  result.ok = true;
  result.commandHash = hash;
  result.canonicalString = canonical;
  result.errorMessage = "";
  return result;
}

// ---------------------------------------------------------------------------
// decideTransaction — duplicate / conflict lookup against journal.
// ---------------------------------------------------------------------------
DecisionResult CommandCanonicalizer::decideTransaction(const String& transactionId,
                                                       const String& commandHash) {
  DecisionResult r;
  r.decision = TransactionDecision::NEW;
  r.previousHash = "";
  r.previousAckJson = "";

  if (!Services::journal.isProcessed(transactionId)) {
    r.decision = TransactionDecision::NEW;
    return r;
  }

  String prevHash = Services::journal.getCommandHash(transactionId);
  String prevAck = Services::journal.getAckJson(transactionId);

  if (prevHash.length() > 0 && prevHash != commandHash) {
    // Same transactionId, different commandHash → CONFLICT.
    // Per directive §16 / AC-010: REJECTED, no physical mutation.
    r.decision = TransactionDecision::CONFLICT;
    r.previousHash = prevHash;
    r.previousAckJson = prevAck;
    return r;
  }

  // Same transactionId, same commandHash (or no previous hash stored) → DUPLICATE.
  // Per directive §16 / AC-009: replay original ACK, no re-execution.
  r.decision = TransactionDecision::DUPLICATE;
  r.previousHash = prevHash;
  r.previousAckJson = prevAck;
  return r;
}

} // namespace Services
