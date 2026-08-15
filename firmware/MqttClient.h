// =============================================================================
// MqttClient.h — MQTT client for remote internet access (CGNAT-friendly)
// =============================================================================
// Connects to MQTT broker (default: HiveMQ public; production: self-hosted
// Mosquitto with TLS + ACL + per-device credentials).
// ESP32 publishes status + logs; subscribes to commands from PWA.
// Works behind CGNAT/MiFi because MQTT uses outbound connection.
//
// Topic structure (R10C-3: password removed from topic — auth via broker creds):
//   timer12/<mac>/status   — ESP32 publishes SystemStatus JSON (every 5s + on-change)
//   timer12/<mac>/command  — PWA publishes command JSON, ESP32 executes
//   timer12/<mac>/log      — ESP32 publishes activity log entries (real-time)
//   timer12/<mac>/online   — ESP32 publishes "1" on connect, "0" on disconnect (LWT)
//   timer12/<mac>/ota      — PWA publishes OTA update commands (signed)
//   timer12/<mac>/ack      — ESP32 publishes ACK for each command (with actual result)
//
// Authentication: via broker username/password (MQTT CONNECT)
// Authorization: via broker ACL (per-device topic restrictions)
//
// Audit round 10E/10F changes:
//   - R10E-1: Atomic ACK transaction (construct → publish → store in one call)
//   - R10E-2: Validation order: type → fields → hash → dedup → execute
//   - R10E-3: Dedup TTL (15min, timestamp-based)
//   - R10F-1: Check publish() return value — don't store if publish fails
//   - R10F-2: Clean up expired dedup entries on discovery (no stale shadowing)
//   - Invalid actions return success:false (not silently ignored)
//   - OTA via MQTT: requires Ed25519 signature + SHA-256 hash + size + requestId
//   - TLS: setCACert() instead of setInsecure() when MQTT_ROOT_CA is set
// =============================================================================
#pragma once
#ifndef TIMER12_MQTT_CLIENT_H
#define TIMER12_MQTT_CLIENT_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "Types.h"  // for Core::LogType

namespace Services {

class MqttClient {
public:
  bool begin();
  void loop();
  bool isConnected();  // non-const: PubSubClient::connected() is non-const
  void publishStatus();
  // CYCLE-7 (fixes F-023): publishLog now accepts explicit logId from LogService
  //   (was using millis() & 0xFFFFFF which collides on wrap and isn't unique per-boot).
  //   If logId=0 (legacy caller), falls back to a module-local monotonic counter.
  void publishLog(Core::LogType type, const String& message, int8_t channelId, uint32_t logId = 0);
  void publishOnline();

private:
  WiFiClient _wifiClient;
  WiFiClientSecure _wifiClientSecure;
  WiFiClientSecure _otaClientSecure;  // separate secure client for OTA HTTPS downloads
  PubSubClient _mqtt;
  String _clientId;
  String _topicStatus;
  String _topicCommand;
  String _topicLog;
  String _topicOnline;
  String _topicOta;
  String _topicAck;
  String _topicAckForJournal;  // R10G-2: stored for publish callback
  unsigned long _lastPublishMs = 0;
  unsigned long _lastReconnectMs = 0;
  bool _initialized = false;

  bool _connect();
  void _onMessage(char* topic, byte* payload, unsigned int length);
  void _handleCommand(const String& json);
  void _handleOta(const String& json);

  // R10E-1 (audit round 10E): ACK publishers now accept commandHash and
  // perform ATOMIC transaction: construct JSON → publish → store in dedup buffer.
  // This eliminates the ordering bug where _addProcessed() was called BEFORE
  // the ACK was published (causing _lastAckJson to be empty/stale).
  //
  // For FAILURE ACKs (success=false), commandHash is "" — failure ACKs are
  // NOT stored in dedup buffer (failed commands can be retried).

  // Generic ACK — failure ACKs (no commandHash = not stored for replay)
  void _publishAck(const String& requestId, bool success, const char* message,
                   const String& dataJson = "", const String& commandHash = "");

  // Success ACKs with type-specific data — all accept commandHash for atomic store
  void _publishRelayAck(const String& requestId, bool success, const char* message,
                        uint8_t channelId, const String& commandHash = "");

  void _publishScheduleAck(const String& requestId, bool success, const char* message,
                           int channelId, int scheduleId, const String& commandHash = "");

  void _publishPirAck(const String& requestId, bool success, const char* message,
                      uint8_t pirId, const String& commandHash = "");

  void _publishChannelAck(const String& requestId, bool success, const char* message,
                          uint8_t channelId, const String& commandHash = "");

  void _publishGenericAck(const String& requestId, bool success, const char* message,
                          const String& dataJson = "", const String& commandHash = "");

  // CYCLE-7 (fixes F-001 + F-002 + F-006): Finalize + publish ACK with proper
  // transaction semantics.
  //
  // preBuiltJson: the fully-constructed ACK JSON to publish.
  // success:      whether the command succeeded (controls commit/publish behavior).
  // commandHash:  non-empty for success ACKs of idempotent commands.
  //
  // Flow:
  //   For FAILURE ACKs (success=false OR commandHash empty):
  //     - Publish immediately (no commit, command can be retried with same requestId).
  //   For SUCCESS ACKs (success=true AND commandHash non-empty):
  //     - Call journal.commitTransaction(requestId, ackJson) — flips PENDING → COMMITTED.
  //     - If commit FAILS: publish FAILURE ACK with DURABILITY_FAILURE message
  //       (do NOT claim success — fixes F-002).
  //     - If commit succeeds: publish ACK immediately.
  //     - If immediate publish succeeds: dequeueAck() to prevent duplicate (fixes F-006).
  //     - If immediate publish fails: leave in queue (processPendingAcks retries).
  void _finalizeAndPublishAck(const String& requestId, bool success,
                               const String& preBuiltJson, const String& commandHash,
                               bool fromPending = false);

  void _buildTopics();
  // audit-fixes: removed _isDuplicate() and _addProcessed() declarations.
  //   The in-memory dedup ring buffer was dead code (never called from
  //   _handleCommand). All dedup is now authoritative via TransactionJournal
  //   (Services::journal.isProcessed() / storeTransaction()).

  // OTA helpers (R10A-2: Ed25519 signature verification via Utils::ed25519VerifyHash)
  bool _downloadAndVerifyOta(const String& url, size_t expectedSize,
                             const char* expectedSha256,
                             const char* expectedSignatureHex,
                             const String& requestId, const String& version);
};

extern MqttClient mqtt;

} // namespace Services

#endif
