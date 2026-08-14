// =============================================================================
// Host-test shim: Arduino.h
// =============================================================================
// Replicates the minimal subset of the Arduino API used by JournalRecord.cpp
// and TransactionJournal.cpp so the SAME source files compile and run on the
// host under g++.
//
// This shim implements:
//   - class String  (length(), c_str(), (const char*, unsigned) ctor,
//                    (const char*) ctor, assignment, ==, !=, indexOf,
//                    substring, toLowerCase — covers JournalRecord +
//                    TransactionJournal usage)
//   - min(a, b)      (template)
//   - Serial         (SerialClass with printf/print/println — for logging)
//   - millis()       (returns std::time(nullptr)*1000 — sufficient for tests)
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
#include <cstdio>
#include <cstdarg>
#include <ctime>

// --- String ----------------------------------------------------------------
class String {
public:
    String() : s_() {}
    String(const char* cstr) : s_(cstr ? cstr : "") {}
    String(const char* cstr, unsigned int length)
        : s_(cstr ? std::string(cstr, length) : std::string(length, '\0')) {}
    String(const std::string& s) : s_(s) {}

    String(const String& other) : s_(other.s_) {}

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

    // --- Additional methods used by TransactionJournal.cpp ---
    int indexOf(char c) const { return (int)s_.find(c); }
    int indexOf(const char* substr) const {
        if (!substr) return -1;
        size_t pos = s_.find(substr);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    String substring(int start) const {
        if (start < 0) start = 0;
        if ((size_t)start > s_.length()) return String();
        return String(s_.substr(start));
    }
    String substring(int start, int end) const {
        if (start < 0) start = 0;
        if (end < start) return String();
        if ((size_t)start > s_.length()) return String();
        if ((size_t)end > s_.length()) end = (int)s_.length();
        return String(s_.substr(start, end - start));
    }
    void toLowerCase() {
        for (auto& c : s_) {
            c = (char)tolower((unsigned char)c);
        }
    }

private:
    std::string s_;
};

// --- min(a, b) -------------------------------------------------------------
template <typename T, typename U>
static inline auto min(const T& a, const U& b) -> decltype(b < a ? b : a) {
    return (b < a) ? b : a;
}

// --- Serial ----------------------------------------------------------------
class __attribute__((unused)) SerialClass {
public:
    void __attribute__((unused)) printf(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    void __attribute__((unused)) print(const char* s) { fputs(s ? s : "", stdout); }
    void __attribute__((unused)) print(const String& s) { fputs(s.c_str(), stdout); }
    void __attribute__((unused)) println(const char* s) { puts(s ? s : ""); }
    void __attribute__((unused)) println(const String& s) { puts(s.c_str()); }
    void __attribute__((unused)) println() { putchar('\n'); }
};
static __attribute__((unused)) SerialClass Serial;

// --- millis() --------------------------------------------------------------
static inline __attribute__((unused)) unsigned long millis() {
    return (unsigned long)(std::time(nullptr) * 1000);
}

// --- HIGH / LOW (used by Config.h's RELAY_ON/RELAY_OFF constants) ----------
// Arduino defines these as 0x0 and 0x1 respectively.
static const uint8_t LOW = 0x00;
static const uint8_t HIGH = 0x01;

#endif // HOST_SHIM_ARDUINO_H
