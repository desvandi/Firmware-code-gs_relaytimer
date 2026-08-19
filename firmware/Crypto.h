// =============================================================================
// Utils/Crypto.h — SHA-256, PBKDF2, HMAC-SHA256, base64url, JWT, random
// =============================================================================
#pragma once
#ifndef TIMER12_UTILS_CRYPTO_H
#define TIMER12_UTILS_CRYPTO_H

#include <Arduino.h>
#include <cstdint>
#include <stddef.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

namespace Utils {

// Constant-time memory compare (for token/hash comparison)
bool constantTimeMemEquals(const volatile uint8_t* a, const volatile uint8_t* b, size_t len);

// Generate cryptographically-random bytes using esp_random()
void generateRandomBytes(uint8_t* buf, size_t len);

// Hex encoding/decoding
void bytesToHex(const uint8_t* in, size_t len, char* out);
bool hexToBytes(const char* hex, uint8_t* out, size_t outLen);

// SHA-256 hash to hex string (64 chars + null)
String sha256Hex(const String& data);

// PBKDF2-HMAC-SHA256 (RFC 8018) — produces 32-byte derived key
bool pbkdf2HmacSha256(const char* pass, size_t passLen,
                      const uint8_t* salt, size_t saltLen,
                      uint16_t iterations, uint8_t* outHash);

// HMAC-SHA256 (single-shot) — used by JWT
bool hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t* outHash);  // out: 32 bytes

// Base64url encoding (no padding) for JWT segments
String base64urlEncode(const uint8_t* data, size_t len);
String base64urlEncode(const String& s);

// JWT (HS256) — sign and verify
// Header: {"alg":"HS256","typ":"JWT"}
// Payload: {"sub":username,"iat":now,"exp":now+ttl}
String jwtSign(const String& username, const String& secret, uint32_t ttlSeconds);
bool jwtVerify(const String& token, const String& secret, String& outUsername);

// Generate random hex token (for CSRF / factory-reset)
String generateToken(size_t hexChars);

// R10A-2 (audit round 10): Ed25519 signature verification for OTA firmware.
// Verifies a 64-byte Ed25519 signature over a 32-byte SHA-256 hash
// (pre-hash mode — signs the hash, not the full binary).
//
// PWA signs: signature = ed25519_sign(sha256(firmware), privateKey)
// ESP32 verifies: ed25519_verify(signature, sha256_hash, publicKey)
//
// publicKeyHex: 64 hex chars (32 raw bytes, Ed25519 public key)
// signatureHex: 128 hex chars (64 raw bytes, Ed25519 signature)
// hashBytes:    32 bytes (SHA-256 of the firmware binary)
//
// Returns true if signature is valid, false otherwise.
// Hard-fails (returns false) if:
//   - MBEDTLS_ED25519_C is not compiled in
//   - public key or signature has wrong length
//   - mbedtls verification returns non-zero
bool ed25519VerifyHash(const char* publicKeyHex,
                       const char* signatureHex,
                       const uint8_t* hashBytes, size_t hashLen);

} // namespace Utils

#endif
