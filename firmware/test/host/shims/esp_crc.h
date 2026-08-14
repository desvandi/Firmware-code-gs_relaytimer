// =============================================================================
// Host-test shim: esp_crc.h
// =============================================================================
// Replicates the ESP-IDF `esp_crc32_le` API for host-side testing.
//
// Why a shim: the firmware's JournalRecord.cpp includes <esp_crc.h> from the
// ESP-IDF SDK, which is unavailable when compiling on the host. The math,
// however, is identical: CRC-32/ISO-HDLC, reflected, polynomial 0xEDB88320.
//
// -----------------------------------------------------------------------------
// IMPORTANT — ESP-IDF esp_crc32_le SEMANTICS (verified against esp_crc.h):
// -----------------------------------------------------------------------------
//   static inline uint32_t esp_crc32_le(uint32_t crc, uint8_t const *buf, size_t len) {
//       for (size_t i = 0; i < len; i++) {
//           crc = crc ^ buf[i];
//           for (uint8_t j = 0; j < 8; j++) {
//               if (crc & 1) {
//                   crc = (crc >> 1) ^ 0xEDB88320;
//               } else {
//                   crc = crc >> 1;
//               }
//           }
//       }
//       return crc;
//   }
//
// Note that esp_crc32_le does NOT complement the input on entry and does NOT
// complement the output on exit. The `crc` parameter is treated as the
// RUNNING CRC STATE directly. The caller is responsible for the standard
// CRC-32 init (0xFFFFFFFF) and final XOR (~) — see firmware's
// verifyCRCGate() and computeRecordCRC().
//
// This means: ~esp_crc32_le(0xFFFFFFFF, "123456789", 9) & 0xFFFFFFFF == 0xCBF43926
// because esp_crc32_le returns the running state (init 0xFFFFFFFF, no final
// XOR), and the outer ~ applies the final XOR.
//
// The table-driven form is mathematically equivalent to the bit-by-bit form
// above (the table pre-computes the 8 rounds of bit-by-bit processing for
// each possible low-byte value).
//
// Self-verification: the test harness includes a known-answer test for the
// "123456789" → 0xCBF43926 vector, so any divergence in this shim fails the
// gate immediately.
//
// This file is ONLY compiled for host tests. The ESP32 build uses the real
// <esp_crc.h> from ESP-IDF; this shim is never flashed to a device.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_ESP_CRC_H
#define HOST_SHIM_ESP_CRC_H

#include <cstdint>
#include <cstddef>

namespace host_shim {

struct CRC32LE {
    uint32_t table[256];

    CRC32LE() {
        // Generate reflected CRC-32 table from poly 0xEDB88320.
        // Bit-by-bit reference algorithm — authoritative definition.
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320u;
                } else {
                    crc >>= 1;
                }
            }
            table[i] = crc;
        }
    }
};

inline const CRC32LE& crcTable() {
    static CRC32LE t;
    return t;
}

} // namespace host_shim

// ESP-IDF-compatible API. Direct running-state semantics: input `crc` is the
// running state (NOT complemented), output is the updated running state
// (NOT complemented). Caller applies init=0xFFFFFFFF and final ~ for the
// standard CRC-32 result.
static inline uint32_t esp_crc32_le(uint32_t crc, const uint8_t* data, size_t len) {
    const uint32_t* t = host_shim::crcTable().table;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ t[(crc ^ data[i]) & 0xFF];
    }
    return crc;
}

#endif // HOST_SHIM_ESP_CRC_H
