// =============================================================================
// Services/OtaManager.h — OTA firmware update with signature verification stub
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_OTA_H
#define TIMER12_SERVICES_OTA_H

#include <Arduino.h>
#include <WebServer.h>

namespace Services {

class OtaManager {
public:
  void begin();
  // Handle upload chunk (called from Web handler)
  void handleUpload(WebServer& server, String filename, size_t index,
                    uint8_t* data, size_t len, bool final);
  // Returns true if update succeeded
  bool isUpdating() const { return _updating; }
  // Latest available version (mock — would query GitHub Release API in production)
  String getLatestVersion() const;
  bool checkUpdateAvailable() const;

private:
  bool _updating = false;
  size_t _totalReceived = 0;
  uint32_t _startTime = 0;
};

extern OtaManager ota;

} // namespace Services

#endif
