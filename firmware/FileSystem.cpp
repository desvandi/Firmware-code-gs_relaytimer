// =============================================================================
// Storage/FileSystem.cpp
// =============================================================================
#include "FileSystem.h"
#include "Config.h"
#include <esp_task_wdt.h>

namespace Storage {

FileSystem fs;

bool FileSystem::begin() {
  if (!LittleFS.begin(false)) {
    if (!LittleFS.format()) {
      return false;
    }
    if (!LittleFS.begin(false)) {
      return false;
    }
  }
  return true;
}

void FileSystem::cleanupTempFiles() {
  if (LittleFS.exists(Core::PATH_SCHEDULE_TMP)) LittleFS.remove(Core::PATH_SCHEDULE_TMP);
  if (LittleFS.exists(Core::PATH_SCHEDULE_BAK_TMP)) LittleFS.remove(Core::PATH_SCHEDULE_BAK_TMP);
  if (LittleFS.exists(Core::PATH_CONFIG_TMP)) LittleFS.remove(Core::PATH_CONFIG_TMP);
}

bool FileSystem::exists(const char* path) {
  return LittleFS.exists(path);
}

File FileSystem::open(const char* path, const char* mode) {
  return LittleFS.open(path, mode);
}

bool FileSystem::remove(const char* path) {
  return LittleFS.remove(path);
}

bool FileSystem::rename(const char* from, const char* to) {
  return LittleFS.rename(from, to);
}

bool FileSystem::atomicWrite(const char* path, const String& content) {
  String tmpPath = String(path) + ".tmp";
  String bakPath = String(path) + ".bak";

  File tmp = LittleFS.open(tmpPath.c_str(), "w");
  if (!tmp) return false;
  tmp.print(content);
  tmp.close();
  esp_task_wdt_reset();

  // Create/update backup
  if (LittleFS.exists(bakPath.c_str())) LittleFS.remove(bakPath.c_str());
  if (LittleFS.exists(path)) LittleFS.rename(path, bakPath.c_str());

  // Promote tmp -> path
  if (!LittleFS.rename(tmpPath.c_str(), path)) {
    // Recovery: try direct write
    if (LittleFS.exists(bakPath.c_str())) {
      LittleFS.rename(bakPath.c_str(), path);
    }
    File direct = LittleFS.open(path, "w");
    if (direct) {
      direct.print(content);
      direct.close();
      return true;
    }
    return false;
  }
  return true;
}

String FileSystem::readAll(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

} // namespace Storage
