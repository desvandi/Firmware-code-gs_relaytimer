// =============================================================================
// Storage/FileSystem.h — LittleFS wrapper (mount, format, atomic write)
// =============================================================================
#pragma once
#ifndef TIMER12_STORAGE_FS_H
#define TIMER12_STORAGE_FS_H

#include <Arduino.h>
#include <LittleFS.h>

namespace Storage {

class FileSystem {
public:
  bool begin();                  // mount, format on failure
  void cleanupTempFiles();
  bool exists(const char* path);
  File open(const char* path, const char* mode);
  bool remove(const char* path);
  bool rename(const char* from, const char* to);

  // Atomic write: write to path.tmp, rename to path (with backup to path.bak)
  bool atomicWrite(const char* path, const String& content);
  String readAll(const char* path);
};

extern FileSystem fs;

} // namespace Storage

#endif
