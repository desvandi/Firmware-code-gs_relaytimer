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
  void resetChannels();
  void markDirty();
  void clearDirty();

  // AUD-FW-CFG-003 FIX: Energy accounting persistence (was RAM-only, reset on reboot).
  // loadEnergyFromNVS() is called on boot; saveEnergyToNVS() is called periodically
  // (every ~60s if dirty) from the main loop to avoid flash wear on every transition.
  void loadEnergyFromNVS();
  void saveEnergyToNVS();

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
