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
//
// -----------------------------------------------------------------------------
// R5-C2 — GRANULAR FAILURE INJECTION (host-only, test-only API)
//
//   Pass-3 shim only supported a single on/off boolean (`setFailMode`) that
//   caused begin() to fail unconditionally. This was insufficient for
//   candidate-pattern tests that need to fail ONE specific NVS write while
//   leaving the rest of the API working (e.g. fail copy B but allow copy A).
//
//   R5-C2 adds a FailMode enum + setFailNextPut / setFailNextGet helpers so
//   tests can request "the next putBytes for key K fails once, then clear".
//
//   All failure state is process-wide static and is auto-cleared after a
//   single failed call (so test setup never leaks failure state into the
//   next assertion).
// -----------------------------------------------------------------------------
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
        // R5-C2: simulate begin() failure when FAIL_BEGIN is armed.
        if (failModeRef() == FAIL_BEGIN) {
            failModeRef() = FAIL_NONE;  // fail once, then auto-clear
            failKeyRef().clear();
            return false;
        }
        // Legacy boolean fail-mode (CP-4): treat `true` as FAIL_BEGIN so
        // existing tests that call setFailMode(true) keep working.
        if (legacyFailModeRef()) {
            return false;
        }
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
        // R5-C2: granular fail-once injection for a specific key.
        if (failModeRef() == FAIL_PUT_KEY && failKeyRef() == std::string(key)) {
            failModeRef() = FAIL_NONE;  // fail once, then auto-clear
            failKeyRef().clear();
            return 0;
        }
        const uint8_t* bytes = (const uint8_t*)data;
        std::vector<uint8_t> v(bytes, bytes + len);
        storage()[ns_ + key] = v;
        return len;
    }

    size_t getBytes(const char* key, void* buf, size_t maxLen) {
        if (!started_ || !key) return 0;
        // R5-C2: granular fail-once injection for a specific key.
        if (failModeRef() == FAIL_GET_KEY && failKeyRef() == std::string(key)) {
            failModeRef() = FAIL_NONE;  // fail once, then auto-clear
            failKeyRef().clear();
            return 0;
        }
        auto it = storage().find(ns_ + key);
        if (it == storage().end()) return 0;
        const std::vector<uint8_t>& v = it->second;
        size_t toCopy = (v.size() < maxLen) ? v.size() : maxLen;
        memcpy(buf, v.data(), toCopy);
        return toCopy;
    }

    size_t putUChar(const char* key, uint8_t value) {
        if (!started_ || !key || readOnly_) return 0;
        if (failModeRef() == FAIL_PUT_KEY && failKeyRef() == std::string(key)) {
            failModeRef() = FAIL_NONE;
            failKeyRef().clear();
            return 0;
        }
        std::vector<uint8_t> v = { value };
        storage()[ns_ + key] = v;
        return 1;
    }

    uint8_t getUChar(const char* key, uint8_t defaultValue = 0) {
        if (!started_ || !key) return defaultValue;
        if (failModeRef() == FAIL_GET_KEY && failKeyRef() == std::string(key)) {
            failModeRef() = FAIL_NONE;
            failKeyRef().clear();
            return defaultValue;
        }
        auto it = storage().find(ns_ + key);
        if (it == storage().end() || it->second.empty()) return defaultValue;
        return it->second[0];
    }

    // uint32_t (used by MqttClient.cpp OTA rate-limiting window timestamp).
    size_t putULong(const char* key, uint32_t value) {
        if (!started_ || !key || readOnly_) return 0;
        if (failModeRef() == FAIL_PUT_KEY && failKeyRef() == std::string(key)) {
            failModeRef() = FAIL_NONE;
            failKeyRef().clear();
            return 0;
        }
        std::vector<uint8_t> v(4);
        v[0] = (uint8_t)(value & 0xFF);
        v[1] = (uint8_t)((value >> 8) & 0xFF);
        v[2] = (uint8_t)((value >> 16) & 0xFF);
        v[3] = (uint8_t)((value >> 24) & 0xFF);
        storage()[ns_ + key] = v;
        return 4;
    }

    uint32_t getULong(const char* key, uint32_t defaultValue = 0) {
        if (!started_ || !key) return defaultValue;
        if (failModeRef() == FAIL_GET_KEY && failKeyRef() == std::string(key)) {
            failModeRef() = FAIL_NONE;
            failKeyRef().clear();
            return defaultValue;
        }
        auto it = storage().find(ns_ + key);
        if (it == storage().end() || it->second.size() < 4) return defaultValue;
        const std::vector<uint8_t>& v = it->second;
        return (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
               ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
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
        // R5-C2: also clear any armed failure injection so tests don't leak state.
        failModeRef() = FAIL_NONE;
        failKeyRef().clear();
        legacyFailModeRef() = false;
    }

    // -------------------------------------------------------------------------
    // R5-C2 — Granular failure injection API
    // -------------------------------------------------------------------------
    enum FailMode {
        FAIL_NONE    = 0,
        FAIL_BEGIN   = 1,  // begin() returns false
        FAIL_PUT_KEY = 2,  // putBytes for specific key fails (once)
        FAIL_GET_KEY = 3,  // getBytes for specific key fails (once)
    };

    static FailMode& failModeRef() {
        static FailMode fm = FAIL_NONE;
        return fm;
    }
    static std::string& failKeyRef() {
        static std::string fk;
        return fk;
    }

    static void setFailMode(FailMode mode) { failModeRef() = mode; }
    static void setFailKey(const std::string& key) { failKeyRef() = key; }
    static void setFailNextPut(const std::string& key) {
        failModeRef() = FAIL_PUT_KEY;
        failKeyRef() = key;
    }
    static void setFailNextGet(const std::string& key) {
        failModeRef() = FAIL_GET_KEY;
        failKeyRef() = key;
    }
    static void clearFailMode() {
        failModeRef() = FAIL_NONE;
        failKeyRef().clear();
    }

    // Legacy CP-4 boolean fail-mode (kept for backward compat with existing tests).
    // When `true`, begin() always returns false (simulates NVS unavailable).
    static void setFailMode(bool fail) { legacyFailModeRef() = fail; }

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

    static bool& legacyFailModeRef() {
        static bool fm = false;
        return fm;
    }
};

#endif // HOST_SHIM_PREFERENCES_H
