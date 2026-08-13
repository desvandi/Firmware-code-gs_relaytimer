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
extern bool relayState[NUM_CHANNELS];
extern RelaySource relaySource[NUM_CHANNELS];
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
