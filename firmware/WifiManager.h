// =============================================================================
// WifiManager.h — ESP32 WiFi STA + Config Portal + AP fallback
// =============================================================================
#pragma once
#ifndef TIMER12_NETWORK_WIFI_H
#define TIMER12_NETWORK_WIFI_H

#include <Arduino.h>
#include <WiFi.h>

namespace TimerNet {

enum class WifiMode {
  STA,        // Station mode (joined to WiFi)
  AP_CONFIG,  // Config Portal AP (waiting for user to input WiFi creds)
  AP_FALLBACK,  // Fallback AP (STA failed, but creds exist)
  NONE
};

class WifiManager {
public:
  bool begin();
  void onEvent(arduino_event_id_t event, arduino_event_info_t info);
  String getApPassword() const;
  IPAddress getLocalIp() const;
  uint8_t getClientCount() const;
  int getRssi() const;
  void generateApPassword();
  WifiMode getMode() const { return _mode; }
  String getMacAddress() const;
  bool isConnected() const;
  String getMqttPassword() const { return _mqttPassword; }
  String getDevicePin() const { return _devicePin; }
  void openConfigPortal();

private:
  char _apPassword[33] = {0};
  char _mqttPassword[9] = {0};
  char _devicePin[7] = {0};
  WifiMode _mode = WifiMode::NONE;
  bool _tryStaMode();
  bool _startApFallback();
  void _loadCredentials();
  void _saveCredentials(const String& ssid, const String& password);
  void _generateMqttPassword();
  void _generateDevicePin();
  void _runConfigPortal();
};

extern WifiManager wifi;

} // namespace TimerNet

#endif
