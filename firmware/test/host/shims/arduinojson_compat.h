// =============================================================================
// arduinojson_compat.h — Test-only ArduinoJson compatibility shim
// =============================================================================
// P2-2 F-P0-2 C3-GATE-002 TEST-INFRASTRUCTURE CORRECTION
//
// Forward-declares ::StringSumHelper so ArduinoJson v6.x's ARDUINOJSON_ENABLE_
// ARDUINO_STRING adapter can specialize IsString<StringSumHelper> without
// requiring the full Arduino String implementation.
//
// This is a TEST-ONLY build artifact (committed under firmware/test/host/
// shims/). Used via `g++ -include shims/arduinojson_compat.h` in the host
// test Makefiles to enable host builds against production source.
//
// Background: ArduinoJson v6.x's ArduinoStringAdapter.hpp references
// ::StringSumHelper via a template trait specialization. The host-test shim
// in firmware/test/host/shims/Arduino.h provides a working ::String class
// but not ::StringSumHelper. Forward-declaring it as an empty class is
// sufficient — the trait is never instantiated because production firmware
// never returns StringSumHelper (it uses operator+= and explicit String
// constructors exclusively).
//
// This is a TEST-INFRASTRUCTURE correction, NOT a C3 semantic fix. It does
// NOT change production behavior, canonical hash schema, MQTT semantics,
// RestJournalHelper contract, or F11 behavior. See HOST_ENV_README.md for
// full C3-GATE-002 disposition.
// =============================================================================
#pragma once
class StringSumHelper;
