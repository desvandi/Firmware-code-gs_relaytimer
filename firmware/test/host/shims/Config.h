// =============================================================================
// Host-test shim: Config.h
// =============================================================================
// Replicates the minimal subset of Core::Config that TransactionJournal.cpp
// uses. Currently only Core::NVS_NAMESPACE is needed.
//
// IMPORTANT: This shim is ONLY compiled for host tests. The ESP32 build uses
// the real Config.h from firmware/; this shim is never flashed to a device.
// =============================================================================
#pragma once
#ifndef HOST_SHIM_CONFIG_H
#define HOST_SHIM_CONFIG_H

namespace Core {
  constexpr const char* NVS_NAMESPACE = "timer12";
}

#endif // HOST_SHIM_CONFIG_H
