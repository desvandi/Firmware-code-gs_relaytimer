// =============================================================================
// Utils/Crc.h — CRC-32 (zlib polynomial) for atomic config integrity
// =============================================================================
#pragma once
#ifndef TIMER12_UTILS_CRC_H
#define TIMER12_UTILS_CRC_H

#include <Arduino.h>
#include <cstdint>
#include <stddef.h>

namespace Utils {

uint32_t calculateCRC(const uint8_t* data, size_t len);

} // namespace Utils

#endif
