// =============================================================================
// Stub: Crc.h (overrides firmware/Crc.h via -I shims)
// =============================================================================
// Crc.h declares Utils::calculateCRC. The impl is in Crc.cpp (which we don't
// compile here). We provide an inline definition so the linker doesn't fail
// if anything pulls in this symbol.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_CRC_H
#define HOST_SHIM_CRC_H

#include <Arduino.h>
#include <cstdint>
#include <cstddef>

namespace Utils {

// CRC-32 (zlib polynomial) — inline stub matching the firmware Crc.cpp impl.
inline uint32_t calculateCRC(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
  }
  return ~crc;
}

} // namespace Utils

#endif // HOST_SHIM_CRC_H
