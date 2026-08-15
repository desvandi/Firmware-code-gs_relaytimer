// =============================================================================
// Storage/ConfigStore.h — Atomic persistence for user config & schedules
// =============================================================================
#pragma once
#ifndef TIMER12_STORAGE_CONFIG_STORE_H
#define TIMER12_STORAGE_CONFIG_STORE_H

#include <Arduino.h>
#include "Types.h"

namespace Storage {

class ConfigStore {
public:
  // ---------- USER CONFIG (auth credentials) ----------
  void loadUserConfig();                       // loads or initializes defaults
  void saveUserConfig();
  void initDefaultUserConfig();                // generates random password from MAC

  // ---------- SCHEDULE / CHANNEL CONFIG ----------
  void loadSchedule();
  void saveSchedule(bool force = false);
  // P2-2 F-P0-2 C3: returns true if save succeeded, false if failed.
  // Callers (REST schedule handlers) need this to decide whether to
  // commit the journal entry or preserve PENDING evidence.
  bool saveScheduleWithResult(bool force = false);
  void resetChannels();
  void markDirty();
  void clearDirty();

  // ---------- DEVICE CONFIG (name, timezone) ----------
  void loadDeviceConfig();
  void saveDeviceConfig();

  // ---------- EXPORT / IMPORT (full backup) ----------
  String exportAll();
  bool importAll(const String& json);
};

extern ConfigStore config;

} // namespace Storage

#endif
