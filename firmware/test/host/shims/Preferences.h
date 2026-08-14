// =============================================================================
// Host-test shim: Preferences.h
// =============================================================================
// Replicates the minimal subset of the ESP32 Preferences (NVS) API used by
// TransactionJournal.cpp so the SAME source file compiles and runs on the
// host under g++.
//
// This shim implements:
//   - class Preferences
//     - begin(namespace, readOnly) → bool
//     - end()
//     - putBytes(key, data, len) → size_t
//     - getBytes(key, buf, maxLen) → size_t
//     - putUChar(key, val) → size_t (1)
//     - getUChar(key, default) → uint8_t
//     - isKey(key) → bool
//     - remove(key) → bool
//
// Storage is process-wide static std::map<string, vector<uint8_t>>.
// Multiple namespaces are supported via prefixing keys with namespace.
//
// IMPORTANT: This shim is ONLY compiled for host tests. The ESP32 build uses
// the real Preferences.h from the arduino-esp32 framework; this shim is never
// flashed to a device.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_PREFERENCES_H
#define HOST_SHIM_PREFERENCES_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

class Preferences {
public:
    Preferences() : ns_(""), started_(false) {}

    bool begin(const char* name, bool readOnly = false) {
        if (!name) return false;
        ns_ = std::string(name) + "/";
        started_ = true;
        readOnly_ = readOnly;
        return true;
    }

    void end() {
        started_ = false;
        ns_.clear();
    }

    bool isKey(const char* key) {
        if (!started_ || !key) return false;
        return storage().find(ns_ + key) != storage().end();
    }

    size_t putBytes(const char* key, const void* data, size_t len) {
        if (!started_ || !key || readOnly_) return 0;
        const uint8_t* bytes = (const uint8_t*)data;
        std::vector<uint8_t> v(bytes, bytes + len);
        storage()[ns_ + key] = v;
        return len;
    }

    size_t getBytes(const char* key, void* buf, size_t maxLen) {
        if (!started_ || !key) return 0;
        auto it = storage().find(ns_ + key);
        if (it == storage().end()) return 0;
        const std::vector<uint8_t>& v = it->second;
        size_t toCopy = (v.size() < maxLen) ? v.size() : maxLen;
        memcpy(buf, v.data(), toCopy);
        return toCopy;
    }

    size_t putUChar(const char* key, uint8_t value) {
        if (!started_ || !key || readOnly_) return 0;
        std::vector<uint8_t> v = { value };
        storage()[ns_ + key] = v;
        return 1;
    }

    uint8_t getUChar(const char* key, uint8_t defaultValue = 0) {
        if (!started_ || !key) return defaultValue;
        auto it = storage().find(ns_ + key);
        if (it == storage().end() || it->second.empty()) return defaultValue;
        return it->second[0];
    }

    bool remove(const char* key) {
        if (!started_ || !key || readOnly_) return false;
        auto it = storage().find(ns_ + key);
        if (it == storage().end()) return false;
        storage().erase(it);
        return true;
    }

    // Test-only: clear all storage (between tests)
    static void clearAllStorage() {
        storage().clear();
    }

private:
    std::string ns_;
    bool started_;
    bool readOnly_;

    // Process-wide static storage — persists across Preferences begin/end
    // within a single test run, but is reset by clearAllStorage().
    static std::unordered_map<std::string, std::vector<uint8_t>>& storage() {
        static std::unordered_map<std::string, std::vector<uint8_t>> s;
        return s;
    }
};

#endif // HOST_SHIM_PREFERENCES_H
