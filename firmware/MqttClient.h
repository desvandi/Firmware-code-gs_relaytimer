// =============================================================================
// MqttClient.h — MQTT client for remote internet access (CGNAT-friendly)
// =============================================================================
// Connects to MQTT broker (default: HiveMQ public; production: self-hosted
// Mosquitto with TLS + ACL + per-device credentials).
// ESP32 publishes status + logs; subscribes to commands from PWA.
// Works behind CGNAT/MiFi because MQTT uses outbound connection.
//
// Topic structure (unique per device, based on MAC address):
//   timer12/<mac>/<mqttPass>/status   — ESP32 publishes SystemStatus JSON (every 5s + on-change)
//   timer12/<mac>/<mqttPass>/command  — PWA publishes command JSON, ESP32 executes
//   timer12/<mac>/<mqttPass>/log      — ESP32 publishes activity log entries (real-time)
//   timer12/<mac>/<mqttPass>/online   — ESP32 publishes "1" on connect, "0" on disconnect (LWT)
//   timer12/<mac>/<mqttPass>/ota      — PWA publishes OTA update commands (signed)
//   timer12/<mac>/<mqttPass>/ack      — ESP32 publishes ACK for each command (with actual result)
//
// Audit round 9 changes:
//   - Dedup buffer: requestId added to buffer ONLY after successful execution
//   - Duplicate ACK: includes actual relay state (channelId/state/source/modeAuto)
//   - All mutations (relay/schedule/pir/channel/time/system/config) send ACK with data
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
  void publishLog(Core::LogType type, const String& message, int8_t channelId);
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
  unsigned long _lastPublishMs = 0;
  unsigned long _lastReconnectMs = 0;
  bool _initialized = false;

  bool _connect();
  void _onMessage(char* topic, byte* payload, unsigned int length);
  void _handleCommand(const String& json);
  void _handleOta(const String& json);

  // Generic ACK publisher — accepts optional data JSON string.
  // Used for ALL mutation ACKs (relay, schedule, pir, channel, time, system, config, ota).
  void _publishAck(const String& requestId, bool success, const char* message,
                   const String& dataJson = "");

  // Relay-specific ACK: looks up actual state and includes it in data.
  // Used for both first-execution and duplicate ACKs (P0 #2 — audit round 9).
  void _publishRelayAck(const String& requestId, bool success, const char* message,
                        uint8_t channelId);

  // Schedule ACK: includes the schedule object that was upserted/deleted.
  void _publishScheduleAck(const String& requestId, bool success, const char* message,
                           int channelId, int scheduleId);

  // PIR ACK: includes the PIR state after config/test.
  void _publishPirAck(const String& requestId, bool success, const char* message,
                      uint8_t pirId);

  // Channel ACK: includes the renamed channel.
  void _publishChannelAck(const String& requestId, bool success, const char* message,
                          uint8_t channelId);

  // Generic config ACK (time set, device config, system reboot, etc.)
  void _publishGenericAck(const String& requestId, bool success, const char* message,
                          const String& dataJson = "");

  void _buildTopics();
  bool _isDuplicate(const String& requestId);
  void _addProcessed(const String& requestId, const String& commandHash = "");

  // OTA helpers (R10A-2: Ed25519 signature verification via Utils::ed25519VerifyHash)
  bool _downloadAndVerifyOta(const String& url, size_t expectedSize,
                             const char* expectedSha256,
                             const char* expectedSignatureHex,
                             const String& requestId, const String& version);
};

extern MqttClient mqtt;

} // namespace Services

#endif
