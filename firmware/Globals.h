// =============================================================================
// Core/Globals.h — Extern declarations for shared mutable state
// Timer Digital Relay v4.0
// =============================================================================
#pragma once
#ifndef TIMER12_CORE_GLOBALS_H
#define TIMER12_CORE_GLOBALS_H

#include "Types.h"

namespace Core {

// ---------- GLOBAL MUTABLE STATE ----------
// Defined once in main .ino; declared extern here for other modules.
extern Channel channels[NUM_CHANNELS];
extern bool relayState[NUM_CHANNELS];        // software commanded state (GPIO output)
extern RelaySource relaySource[NUM_CHANNELS];
// v4.3 audit P1-005, P1-014: physical state tracking (UNKNOWN without aux feedback)
extern bool relayPhysicalState[NUM_CHANNELS];  // last confirmed physical state
                                                // (false until aux contact feedback available)
extern StateConfidence relayStateConfidence[NUM_CHANNELS];
extern uint32_t relayStateSequence[NUM_CHANNELS];  // monotonic per-channel sequence
extern unsigned long relayStateTimestamp[NUM_CHANNELS];  // last state change ms
extern bool relayFault[NUM_CHANNELS];               // state drift or interlock violation
extern PirState pirState[NUM_PIR];
extern UserConfig userConfig;
extern SystemMetrics metrics;
extern bool timeValid;
extern bool scheduleDirty;
extern bool firstDirtySet;
extern unsigned long lastSaveTime;
extern unsigned long firstDirtyTime;
extern unsigned long pirStartupTime;
extern char csrfToken[CSRF_TOKEN_LEN + 1];
extern unsigned long csrfTokenTime;
extern char apPassword[33];
extern char deviceName[33];
extern char timezone[40];

// JWT secret (loaded from NVS in production; compile-time default here)
extern char jwtSecret[65];

// Auth attempts (rate limiter)
extern AuthAttempt authAttempts[MAX_TRACKED_IPS];

// Factory reset token (in-RAM only — lost on reboot, by design)
extern char factoryResetToken[33];
extern unsigned long factoryResetTokenTime;

} // namespace Core

#endif // TIMER12_CORE_GLOBALS_H
