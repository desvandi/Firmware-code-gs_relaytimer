// =============================================================================
// Host-test shim: Arduino.h
// =============================================================================
// Replicates the minimal subset of the Arduino API used by JournalRecord.cpp
// so the SAME source file compiles and runs on the host under g++.
//
// This shim ONLY implements:
//   - class String  (length(), c_str(), (const char*, unsigned) ctor,
//                   (const char*) ctor, assignment from const char*,
//                   ==, !=, default ctor)
//   - min(a, b)     (template; matches Arduino's macro semantics)
//
// No other Arduino features (Serial, delay, PROGMEM, etc.) are needed for
// Phase 1 JournalRecord testing. If a future phase requires more, extend
// this shim rather than reaching for the real Arduino.h.
//
// IMPORTANT: This shim is ONLY compiled for host tests. The ESP32 build uses
// the real Arduino.h from the arduino-esp32 framework; this shim is never
// flashed to a device.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_ARDUINO_H
#define HOST_SHIM_ARDUINO_H

#include <string>
#include <cstdint>

// --- String ----------------------------------------------------------------
// Internally backed by std::string so behaviour matches Arduino's String for
// the operations JournalRecord.cpp uses (length, c_str, binary-safe ctor,
// assignment, equality). Arduino's String is null-terminated; this shim
// preserves that contract — embedded nulls are not a use case for Phase 1.
class String {
public:
    String() : s_() {}
    String(const char* cstr) : s_(cstr ? cstr : "") {}
    String(const char* cstr, unsigned int length)
        : s_(cstr ? std::string(cstr, length) : std::string(length, '\0')) {}
    String(const std::string& s) : s_(s) {}

    // Explicit copy constructor (silences -Wdeprecated-copy under -Werror).
    String(const String& other) : s_(other.s_) {}

    // Assignment from const char* (Arduino supports this).
    String& operator=(const char* cstr) {
        s_ = (cstr ? cstr : "");
        return *this;
    }
    String& operator=(const String& other) {
        s_ = other.s_;
        return *this;
    }

    unsigned int length() const { return static_cast<unsigned int>(s_.length()); }
    const char* c_str() const { return s_.c_str(); }

    bool operator==(const String& other) const { return s_ == other.s_; }
    bool operator==(const char* other) const {
        return (other != nullptr) && (s_ == other);
    }
    bool operator!=(const String& other) const { return !(*this == other); }
    bool operator!=(const char* other) const { return !(*this == other); }

private:
    std::string s_;
};

// --- min(a, b) -------------------------------------------------------------
// Arduino defines min() as a macro / template; here it's a plain template.
template <typename T, typename U>
static inline auto min(const T& a, const U& b) -> decltype(b < a ? b : a) {
    return (b < a) ? b : a;
}

#endif // HOST_SHIM_ARDUINO_H
