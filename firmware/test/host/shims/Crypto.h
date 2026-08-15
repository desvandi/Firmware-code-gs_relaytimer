// =============================================================================
// Stub: Crypto.h (overrides firmware/Crypto.h via -I shims)
// =============================================================================
// MqttClient.cpp uses Utils::sha256Hex (for command hash) and
// Utils::bytesToHex + Utils::ed25519VerifyHash (in OTA download path —
// only reached if OTA_HTTPS_ROOT_CA is non-empty, which tests don't do).
//
// We provide:
//   - REAL SHA-256 via OpenSSL (Utils::sha256Hex + Utils::bytesToHex)
//   - Stub Utils::ed25519VerifyHash → returns false (forces OTA fail-closed)
//   - Stubs for all other crypto functions (not exercised by MqttClient)
// =============================================================================
#pragma once
#ifndef HOST_SHIM_CRYPTO_H
#define HOST_SHIM_CRYPTO_H

#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include <openssl/sha.h>

namespace Utils {

// Convert raw bytes to lowercase hex string (matches Crypto.cpp impl).
inline void bytesToHex(const uint8_t* in, size_t len, char* out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2]     = hex[(in[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[in[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

// REAL SHA-256 (OpenSSL) — matches firmware's mbedtls-based impl.
inline String sha256Hex(const String& data) {
  uint8_t hash[32];
  SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash);
  char hex[65];
  bytesToHex(hash, 32, hex);
  return String(hex);
}

inline bool constantTimeMemEquals(const volatile uint8_t*, const volatile uint8_t*, size_t) {
  return false;
}
inline void generateRandomBytes(uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i & 0xFF);
}
inline bool hexToBytes(const char*, uint8_t*, size_t) { return false; }
inline bool pbkdf2HmacSha256(const char*, size_t, const uint8_t*, size_t, uint16_t, uint8_t*) {
  return false;
}
inline bool hmacSha256(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) {
  return false;
}
inline String base64urlEncode(const uint8_t*, size_t) { return ""; }
inline String base64urlEncode(const String&) { return ""; }
inline String jwtSign(const String&, const String&, uint32_t) { return ""; }
inline bool jwtVerify(const String&, const String&, String&) { return false; }
inline String generateToken(size_t) { return ""; }

// Ed25519 stub — not exercised in tests (OTA fails before reaching this).
inline bool ed25519VerifyHash(const char*, const char*, const uint8_t*, size_t) {
  return false;
}

} // namespace Utils

#endif // HOST_SHIM_CRYPTO_H
