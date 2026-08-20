// =============================================================================
// ed25519-donna.h — Self-contained Ed25519 signature verification (public domain)
// -----------------------------------------------------------------------------
// AUD-FW-OTA-001 FIX (Opsi B): Mengganti PSA Crypto (yang tidak kompatibel
// dengan C++) dengan implementasi Ed25519 yang self-contained dalam C murni.
//
// Library ini hanya butuh:
//   - mbedtls/sha512.h (sudah ada di arduino-esp32 framework, kompatibel C++)
//   - Tidak butuh PSA Crypto headers
//   - Tidak butuh framework rebuild
//   - Tidak butuh CONFIG_MBEDTLS_ECP_DP_ED25519_ENABLED
//
// API:
//   int ed25519_donna_verify(const uint8_t *sig, const uint8_t *pub, const uint8_t *msg, size_t msglen)
//   Returns 1 if signature is valid, 0 if invalid.
//
// Reference: RFC 8032 §5.1.7 (Verify)
// Algorithm: https://tools.ietf.org/html/rfc8032#section-5.1.7
// =============================================================================
#pragma once
#ifndef TIMER12_ED25519_DONNA_H
#define TIMER12_ED25519_DONNA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Verify an Ed25519 signature.
//   sig: 64-byte signature
//   pub: 32-byte public key
//   msg: message bytes (for OTA: the SHA-256 hash of firmware, 32 bytes)
//   msglen: message length
// Returns: 1 = valid, 0 = invalid
int ed25519_donna_verify(const uint8_t *sig, const uint8_t *pub,
                         const uint8_t *msg, size_t msglen);

#ifdef __cplusplus
}
#endif

#endif // TIMER12_ED25519_DONNA_H
