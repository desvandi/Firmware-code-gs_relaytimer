// =============================================================================
// Host-test shim: Arduino.h
// =============================================================================
// Replicates the minimal subset of the Arduino API used by JournalRecord.cpp,
// TransactionJournal.cpp, and MqttClient.cpp so the SAME source files compile
// and run on the host under g++.
//
// This shim implements:
//   - class String  (full set of constructors, operators, and methods used by
//                    the firmware: int/unsigned long/char ctors, operator+,
//                    operator+=, operator[], charAt, endsWith, trim, reserve,
//                    toInt, indexOf, substring, toLowerCase — all delegating
//                    to std::string internally)
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
#include <cctype>
#include <cstring>
#include <ctime>
#include <cstdlib>

// --- String ----------------------------------------------------------------
class String {
public:
    String() : s_() {}
    String(const char* cstr) : s_(cstr ? cstr : "") {}
    String(const char* cstr, unsigned int length)
        : s_(cstr ? std::string(cstr, length) : std::string(length, '\0')) {}
    String(const std::string& s) : s_(s) {}

    // Numeric constructors (used by MqttClient.cpp: String(channelId), String((unsigned long)(...)))
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned int v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    String(float v) : s_(std::to_string(v)) {}
    String(double v) : s_(std::to_string(v)) {}
    String(char c) : s_(1, c) {}

    String(const String& other) : s_(other.s_) {}

    String& operator=(const char* cstr) {
        s_ = (cstr ? cstr : "");
        return *this;
    }
    String& operator=(const String& other) {
        s_ = other.s_;
        return *this;
    }
    String& operator=(const std::string& other) {
        s_ = other;
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

    // --- Concatenation operators (member) ---
    String& operator+=(const String& other) { s_ += other.s_; return *this; }
    String& operator+=(const char* cstr) {
        if (cstr) s_ += cstr;
        return *this;
    }
    String& operator+=(char c) { s_ += c; return *this; }
    String& operator+=(int v) { s_ += std::to_string(v); return *this; }
    String& operator+=(unsigned int v) { s_ += std::to_string(v); return *this; }
    String& operator+=(long v) { s_ += std::to_string(v); return *this; }
    String& operator+=(unsigned long v) { s_ += std::to_string(v); return *this; }

    // --- Indexing ---
    char operator[](unsigned int idx) const {
        return (idx < s_.length()) ? s_[idx] : '\0';
    }
    char& operator[](unsigned int idx) {
        static char dummy = '\0';
        return (idx < s_.length()) ? s_[idx] : dummy;
    }

    // --- indexOf / substring / case ---
    int indexOf(char c) const { return (int)s_.find(c); }
    int indexOf(char c, int startPos) const {
        if (startPos < 0 || (size_t)startPos > s_.length()) return -1;
        size_t pos = s_.find(c, (size_t)startPos);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(const char* substr) const {
        if (!substr) return -1;
        size_t pos = s_.find(substr);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(const char* substr, int startPos) const {
        if (!substr || startPos < 0 || (size_t)startPos > s_.length()) return -1;
        size_t pos = s_.find(substr, (size_t)startPos);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(const String& substr) const {
        size_t pos = s_.find(substr.s_);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(const String& substr, int startPos) const {
        if (startPos < 0 || (size_t)startPos > s_.length()) return -1;
        size_t pos = s_.find(substr.s_, (size_t)startPos);
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
    void toUpperCase() {
        for (auto& c : s_) {
            c = (char)toupper((unsigned char)c);
        }
    }

    // --- Arduino-compat helpers ---
    char charAt(unsigned int idx) const { return operator[](idx); }
    bool endsWith(const String& suffix) const {
        if (suffix.length() > length()) return false;
        return s_.compare(s_.length() - suffix.length(), suffix.length(), suffix.s_) == 0;
    }
    bool endsWith(const char* suffix) const {
        if (!suffix) return false;
        size_t suffixLen = strlen(suffix);
        if (suffixLen > s_.length()) return false;
        return s_.compare(s_.length() - suffixLen, suffixLen, suffix) == 0;
    }
    bool startsWith(const String& prefix) const {
        if (prefix.length() > length()) return false;
        return s_.compare(0, prefix.length(), prefix.s_) == 0;
    }
    bool startsWith(const char* prefix) const {
        if (!prefix) return false;
        size_t prefixLen = strlen(prefix);
        if (prefixLen > s_.length()) return false;
        return s_.compare(0, prefixLen, prefix) == 0;
    }
    void trim() {
        size_t start = s_.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) {
            s_.clear();
            return;
        }
        size_t end = s_.find_last_not_of(" \t\n\r");
        s_ = s_.substr(start, end - start + 1);
    }
    void reserve(size_t capacity) { s_.reserve(capacity); }
    long toInt() const { return std::strtol(s_.c_str(), nullptr, 10); }
    unsigned long toUnsignedLong() const { return std::strtoul(s_.c_str(), nullptr, 10); }
    double toDouble() const { return std::strtod(s_.c_str(), nullptr); }
    float toFloat() const { return (float)toDouble(); }

    // --- Arduino Print / concat protocol (used by ArduinoJson writers) ---
    // ArduinoJson's ArduinoStringWriter calls `destination_->concat(buffer_)`
    // to append serialized JSON to a String. Returns true on success.
    bool concat(const char* cstr) {
        if (cstr) s_ += cstr;
        return true;
    }
    bool concat(const String& other) {
        s_ += other.s_;
        return true;
    }
    bool concat(char c) {
        s_ += c;
        return true;
    }
    bool concat(unsigned char c) {
        s_ += std::to_string((int)c);
        return true;
    }
    bool concat(int v) {
        s_ += std::to_string(v);
        return true;
    }
    bool concat(unsigned int v) {
        s_ += std::to_string(v);
        return true;
    }
    bool concat(long v) {
        s_ += std::to_string(v);
        return true;
    }
    bool concat(unsigned long v) {
        s_ += std::to_string(v);
        return true;
    }
    bool concat(float v) {
        s_ += std::to_string(v);
        return true;
    }
    bool concat(double v) {
        s_ += std::to_string(v);
        return true;
    }
    // Arduino Print interface: write() methods. Required if ArduinoJson's
    // default Writer template is used (when ARDUINOJSON_ENABLE_ARDUINO_STRING
    // is 0). For safety, we provide them.
    size_t write(uint8_t c) {
        s_ += (char)c;
        return 1;
    }
    size_t write(const uint8_t* s, size_t n) {
        if (s) s_.append(reinterpret_cast<const char*>(s), n);
        return n;
    }

private:
    std::string s_;
};

// --- Free-form concatenation operators (String + various) ---
inline String operator+(const String& lhs, const String& rhs) {
    String result(lhs);
    result += rhs;
    return result;
}
inline String operator+(const String& lhs, const char* rhs) {
    String result(lhs);
    result += rhs;
    return result;
}
inline String operator+(const char* lhs, const String& rhs) {
    String result(lhs ? lhs : "");
    result += rhs;
    return result;
}
inline String operator+(const String& lhs, char rhs) {
    String result(lhs);
    result += rhs;
    return result;
}
inline String operator+(const String& lhs, int rhs) {
    String result(lhs);
    result += rhs;
    return result;
}
inline String operator+(const String& lhs, unsigned int rhs) {
    String result(lhs);
    result += rhs;
    return result;
}
inline String operator+(const String& lhs, long rhs) {
    String result(lhs);
    result += rhs;
    return result;
}
inline String operator+(const String& lhs, unsigned long rhs) {
    String result(lhs);
    result += rhs;
    return result;
}

// --- min(a, b) / max(a, b) -------------------------------------------------
template <typename T, typename U>
static inline auto min(const T& a, const U& b) -> decltype(b < a ? b : a) {
    return (b < a) ? b : a;
}
template <typename T, typename U>
static inline auto max(const T& a, const U& b) -> decltype(b < a ? a : b) {
    return (b < a) ? a : b;
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
    void __attribute__((unused)) print(int v) { printf("%d", v); }
    void __attribute__((unused)) print(unsigned long v) { printf("%lu", v); }
    void __attribute__((unused)) println(const char* s) { puts(s ? s : ""); }
    void __attribute__((unused)) println(const String& s) { puts(s.c_str()); }
    void __attribute__((unused)) println(int v) { printf("%d\n", v); }
    void __attribute__((unused)) println(unsigned long v) { printf("%lu\n", v); }
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
