# Safety Case — Timer Digital Relay v4.2

> Implements safety case requirements from the Industrial-Grade
> Implementation Directive §101, §113.

Format per brief §101: HAZARD / CAUSE / RISK / PREVENTION / DETECTION /
SAFE STATE / RECOVERY / TEST.

---

## HAZARD: HTR-001 — Heater remains ON unattended

| Field | Value |
|---|---|
| **Hazard** | Heater (resistive load) energized indefinitely, causing fire |
| **Cause** | (a) Software/network failure preventing OFF command (b) Operator forgets to turn off (c) Schedule failure (RTC invalid, schedule not executing) |
| **Risk** | HIGH — property damage, possible injury/death from fire |
| **Prevention** | `maxOnTimeSec` configured per channel (default: 0=unlimited; recommended: 7200=2h for heaters). Auto FORCE OFF after limit. |
| **Detection** | (a) AlarmRegistry raises `ERR_RELAY_002` (maxOnTime) at CRITICAL severity (b) Activity log entry "FORCE OFF — maxOnTime exceeded" (c) Health Supervisor records uptime for forensic analysis |
| **Safe state** | Channel forced OFF, maxOnTimeForced=true. Operator must manually toggle to clear. |
| **Recovery** | (1) Investigate why command was prevented (2) Operator acknowledges alarm (3) Manually toggle ON (clears maxOnTimeForced flag) (4) Schedule resumed if applicable |
| **Test** | `REL-010` — maxOnTime force-OFF after configured seconds (see TEST_PLAN.md) |

---

## HAZARD: MTR-001 — Motor rapid cycling (chatter) damages windings

| Field | Value |
|---|---|
| **Hazard** | Motor (inductive load) cycled ON/OFF rapidly, causing contactor arcing + winding insulation damage |
| **Cause** | (a) Noisy PIR sensor toggles (b) Unstable sensor signal (c) Network command flood from PWA |
| **Risk** | MEDIUM — equipment damage, possible fire if contactor welds |
| **Prevention** | `minSwitchIntervalSec` configured per channel (default: 0; recommended: 5-10s for motors). Anti-chatter filter blocks rapid transitions. |
| **Detection** | AlarmRegistry raises `ERR_RELAY_005` (anti-chatter) at INFO severity (throttled to 1 per 5s to avoid spam) |
| **Safe state** | Transition blocked, current state preserved |
| **Recovery** | Once interval elapsed, normal transitions resume. No operator intervention needed. |
| **Test** | `REL-003` — Rapid ON/OFF (anti-chatter test) |

---

## HAZARD: REL-001 — Relay contact welded shut

| Field | Value |
|---|---|
| **Hazard** | Relay contact physically stuck CLOSED (welded) — load remains energized even when GPIO=OFF |
| **Cause** | (a) Sustained overcurrent through contact (b) Arcing on inductive load without snubber (c) Old age / mechanical wear |
| **Risk** | HIGH — load cannot be de-energized, potential fire/electrocution |
| **Prevention** | (a) Use properly rated contactors for the load (b) Install RC snubber / flyback diode on inductive loads (c) Use NC (Normally Closed) contacts for fail-safe loads where applicable |
| **Detection** | (Current implementation) NOT DETECTABLE — no auxiliary contact feedback. PWA shows commanded state, not physical. (Future) Add current sensor on load side → verify current flow matches GPIO command. |
| **Safe state** | N/A — software cannot detect this. **Operator must verify load is OFF before servicing.** |
| **Recovery** | Manual: replace relay module. Verify load is genuinely de-energized with multimeter before opening enclosure. |
| **Test** | NOT EXECUTABLE — requires physical contact feedback (future hardware revision) |
| **Documentation** | HARDWARE_SAFETY_CONTRACT.md §9 — "Physical Feedback Disclosure" |

---

## HAZARD: RTC-001 — RTC battery dead, time invalid

| Field | Value |
|---|---|
| **Hazard** | Scheduler executes wrong schedule (e.g., lights ON at midnight instead of 18:00) |
| **Cause** | DS3231 CR2032 backup battery depleted after ~5 years |
| **Risk** | LOW — inconvenience, not safety-critical |
| **Prevention** | (a) Replace DS3231 CR2032 annually (preventative maintenance) (b) Sync RTC from PWA weekly (manual) |
| **Detection** | (a) RTC state machine VALID/INVALID/UNSYNCED (brief §18) (b) AlarmRegistry raises `ERR_RTC_001` (invalid) at CRITICAL severity (c) Health Supervisor reports `rtcStatus: INVALID` |
| **Safe state** | Scheduler INHIBITED — no time-based schedules execute. Manual + PIR overrides still work. |
| **Recovery** | (1) Sync RTC via PWA → Settings → Set RTC Time → Sync Now (2) Verify `rtcStatus: VALID` (3) Schedules resume automatically |
| **Test** | `PL-021` — RTC invalid (battery dead) |

---

## HAZARD: PWR-001 — Power loss during relay transition

| Field | Value |
|---|---|
| **Hazard** | Relay state indeterminate after power loss — load may be ON or OFF unexpectedly |
| **Cause** | Power loss during the microsecond window between GPIO write and physical contact closure |
| **Risk** | LOW — depends on load. For loads that must auto-resume (refrigerator), use RESTORE_LAST boot policy. For hazardous loads (heater), use BOOT_OFF. |
| **Prevention** | Per-channel `bootPolicy`: BOOT_OFF for hazardous, RESTORE_LAST for resumable, SAFE_STATE for mixed |
| **Detection** | Boot log shows `lastResetReason=POWERON_RESET` (or `EXT_RESET`). Boot policy applies desired state explicitly. |
| **Safe state** | BOOT_OFF default — all hazardous loads de-energized on boot |
| **Recovery** | After boot, scheduler/PIR/manual control resumes normal operation. Transaction journal replays pending commands idempotently (brief §25). |
| **Test** | `PL-004` — Power loss after physical mutation, `PL-009` — Duplicate command after reboot |

---

## HAZARD: NET-001 — Network failure during command

| Field | Value |
|---|---|
| **Hazard** | Operator's OFF command lost in transit — load remains ON |
| **Cause** | (a) WiFi drop (b) MQTT broker failure (c) Internet outage (CGNAT path) |
| **Risk** | LOW — local automation (scheduler, PIR, maxOnTime) continue without network |
| **Prevention** | (a) maxOnTimeSec configured for hazardous loads (b) MQTT QoS 1 + ACK with retry (c) TransactionJournal stores pending ACKs for NVS-backed retry |
| **Detection** | (a) PWA shows COMMAND_PENDING → TIMEOUT after 5s (b) AlarmRegistry raises `ERR_NET_003` (MQTT publish fail) (c) Health Supervisor tracks `mqttReconnectCount` |
| **Safe state** | Local control continues. Network-dependent features (remote PWA control, AI insights) disabled until reconnect. |
| **Recovery** | (1) ESP32 reconnects with exponential backoff (5s → 10s → 20s → 40s) (2) Pending ACKs replayed from journal (3) PWA reconciles state on reconnect |
| **Test** | `FAULT-001` — MQTT broker unreachable, `PL-024` — WiFi unavailable 24 hours |

---

## HAZARD: OTA-001 — OTA install malicious/forged firmware

| Field | Value |
|---|---|
| **Hazard** | Attacker pushes malicious firmware that disables safety logic |
| **Cause** | (a) Stolen Ed25519 private key (b) Compromised MQTT broker (c) Compromised PWA deployment |
| **Risk** | CRITICAL — full system compromise |
| **Prevention** | (a) Ed25519 signature over SHA-256 hash (not full binary — RAM constraint) (b) Anti-downgrade check (c) URL allowlist (d) HTTPS with root CA validation (e) Private key NEVER in firmware or repo |
| **Detection** | (a) Signature verification fails → OTA aborts (b) AlarmRegistry raises `ERR_OTA_004` (Ed25519 invalid) (c) Activity log "OTA rejected — signature mismatch" |
| **Safe state** | OTA aborts, no flash write, previous firmware retained |
| **Recovery** | (1) If private key compromised: rotate keys immediately (2) Re-flash affected devices via USB with new public key in Config.h (3) Audit activity logs for unauthorized OTA attempts |
| **Test** | `OTA-002` — Invalid signature → rejected, `SEC-015` — OTA with invalid Ed25519 signature |

---

## HAZARD: SEC-001 — Cross-device MQTT access

| Field | Value |
|---|---|
| **Hazard** | Device A can read/control Device B's relays via MQTT |
| **Cause** | (a) Shared MQTT username/password across fleet (b) Broker ACL not configured (c) Topic structure allows wildcards |
| **Risk** | HIGH — attacker with one device credential can control entire fleet |
| **Prevention** | (a) Per-device MQTT username/password (b) Broker ACL restricts each device to its own topic subtree (`timer12/<mac>/*`) (c) PRODUCTION_BUILD flag enforces TLS + non-empty credentials + non-wildcard CORS |
| **Detection** | (a) Broker logs unauthorized topic access attempts (b) PWA won't receive cross-device telemetry (ACL blocks) |
| **Safe state** | Broker rejects unauthorized publish/subscribe |
| **Recovery** | (1) Audit broker logs for cross-device access (2) Rotate compromised device credentials (3) Re-provision device |
| **Test** | `SEC-007` — Wrong MQTT topic (cross-device) → broker ACL reject |

---

## HAZARD: SEC-002 — Replay attack on REST/MQTT command

| Field | Value |
|---|---|
| **Hazard** | Attacker replays a captured relay command, causing unintended relay state change |
| **Cause** | (a) Same `requestId` submitted twice with malicious intent (b) Captured MQTT packet replayed |
| **Risk** | MEDIUM — depends on command. ON/OFF replay is benign (idempotent). Schedule modification replay could cause unintended schedule. |
| **Prevention** | (a) TransactionJournal dedup with 15-min TTL (b) Canonical command hash excludes requestId (so same logical command from different requestId is detected) (c) HMAC on GAS prevents tampering |
| **Detection** | (a) Duplicate requestId → ACK replayed from journal (no physical re-execution) (b) requestId collision (same ID, different payload) → REJECT 409 |
| **Safe state** | Idempotent commands replay safely. Non-idempotent commands rejected on collision. |
| **Recovery** | No recovery needed — duplicate detection is transparent. Operator sees original ACK. |
| **Test** | `SEC-003` — Replayed requestId → 409 CONFLICT or replayed ACK, `SEC-013` — requestId collision → reject |

---

## HAZARD: MEM-001 — Heap exhaustion

| Field | Value |
|---|---|
| **Hazard** | Firmware crashes due to out-of-memory |
| **Cause** | (a) Large JSON allocation (e.g., oversized config import) (b) Memory fragmentation from repeated String allocations (c) MQTT packet too large (v4.1.0 had 4 KB buffer, v4.1.1+ has 16 KB) |
| **Risk** | LOW — watchdog catches and reboots. Recovery automatic. |
| **Prevention** | (a) `requireBody(MAX_BODY_SIZE=16384)` rejects oversized REST bodies (b) ArduinoJson DynamicJsonDocument with fixed size (c) Health Supervisor raises LOW_HEAP alarm at <20 KB free |
| **Detection** | (a) Health Supervisor tracks `freeHeap`, `minFreeHeap`, `largestFreeBlock` (b) LOW_HEAP alarm at <20 KB (c) Boot reason captures crash type (watchdog) |
| **Safe state** | Watchdog resets, firmware reboots with BOOT_OFF for hazardous loads |
| **Recovery** | Automatic — watchdog fires, system reboots. Boot health check rolls back if OTA caused the leak. |
| **Test** | `FAULT-010` — Force watchdog reset |

---

## HAZARD: SEC-003 — Secret leak via repository

| Field | Value |
|---|---|
| **Hazard** | Private key / MQTT password / JWT secret committed to GitHub |
| **Cause** | Developer accidentally commits `.env` file or secrets in source code |
| **Risk** | CRITICAL — full fleet compromise if production secrets leak |
| **Prevention** | (a) All secrets generated per-device at first boot (stored in NVS, never in source) (b) `.env.example` documents required env vars without actual values (c) `.gitignore` excludes `.env` (d) Recommended CI secret scan (trufflehog) per DEPLOYMENT.md §2.5 |
| **Detection** | (a) CI trufflehog scan fails on commit (b) Manual review of git history for secrets (c) GitHub secret scanning (automatic for public repos) |
| **Safe state** | N/A — prevention is the safe state |
| **Recovery** | (1) Immediately rotate ALL leaked secrets (2) Force password reset for affected users (3) Re-flash all devices with new keys (4) Force logout all sessions (5) Audit access logs for misuse |
| **Test** | N/A — prevention is the test (CI scan) |

---

## Summary: Hazard Severity Matrix

| Hazard ID | Hazard | Risk | Mitigation status (v4.2) |
|---|---|---|---|
| HTR-001 | Heater remains ON | HIGH | ✅ maxOnTime FORCE OFF (local-first) |
| MTR-001 | Motor chatter | MEDIUM | ✅ minSwitchInterval anti-chatter |
| REL-001 | Contact welded | HIGH | ⚠️ NOT DETECTABLE without aux feedback (documented limitation) |
| RTC-001 | RTC battery dead | LOW | ✅ State machine + scheduler inhibit |
| PWR-001 | Power loss during transition | LOW | ✅ Per-channel boot policy |
| NET-001 | Network failure | LOW | ✅ Local-first + journal replay |
| OTA-001 | Forged firmware | CRITICAL | ✅ Ed25519 + allowlist + anti-downgrade |
| SEC-001 | Cross-device MQTT | HIGH | ✅ Per-device ACL + PRODUCTION_BUILD enforcement |
| SEC-002 | Replay attack | MEDIUM | ✅ TransactionJournal + canonical hash |
| MEM-001 | Heap exhaustion | LOW | ✅ Watchdog + boot rollback + LOW_HEAP alarm |
| SEC-003 | Secret leak | CRITICAL | ✅ Per-device generation + CI secret scan |

**Production readiness**: This system is production-ready for the
implemented mitigations. The REL-001 hazard (welded contact) is a
hardware limitation documented in HARDWARE_SAFETY_CONTRACT.md §9 —
proper electrical protection (fuses, MCBs) at the load side is mandatory
regardless of software mitigations (brief §113).
