// =============================================================================
// MqttClient.h — MQTT client for remote internet access (CGNAT-friendly)
// =============================================================================
// Connects to HiveMQ public broker (broker.hivemq.com:1883).
// ESP32 publishes status + logs; subscribes to commands from PWA.
// Works behind CGNAT/MiFi because MQTT uses outbound connection.
//
// Topic structure (unique per device, based on MAC address):
//   timer12/<mac>/status   — ESP32 publishes SystemStatus JSON (every 5s + on-change)
//   timer12/<mac>/command  — PWA publishes command JSON, ESP32 executes
//   timer12/<mac>/log      — ESP32 publishes activity log entries (real-time)
//   timer12/<mac>/online   — ESP32 publishes "1" on connect, "0" on disconnect (LWT)
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
  void _publishAck(const String& requestId, bool success, const char* message);
  void _buildTopics();
  bool _isDuplicate(const String& requestId);
  void _addProcessed(const String& requestId);
};

extern MqttClient mqtt;

} // namespace Services

#endif
