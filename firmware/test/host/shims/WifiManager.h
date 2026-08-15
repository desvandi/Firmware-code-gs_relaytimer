// =============================================================================
// Stub: WifiManager.h (overrides firmware/WifiManager.h via -I shims)
// =============================================================================
// MqttClient.cpp uses TimerNet::wifi.getMacAddress() (in _buildTopics),
// .isConnected() (in _connect + loop), and .getRssi() (in publishStatus).
// The full firmware/WifiManager.h includes <WiFi.h> (stubbed). Our stub
// returns deterministic test values.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_WIFI_MANAGER_H
#define HOST_SHIM_WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>

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

extern WifiManager wifi;

} // namespace TimerNet

#endif // HOST_SHIM_WIFI_MANAGER_H
