# Phase 3 — 16-Channel Hardware / Architecture Migration

**Status:** 🔴 NOT AUTHORIZED — placeholder scope document.
Phase 3 may not begin until:
1. Phase 1 is approved by auditor (after Closure-C/D/E/F re-audit).
2. Phase 2 implementation is complete + audited and approved by auditor
   against `docs/PHASE-2-SCOPE.md`.
3. Auditor explicitly authorizes "Phase 3 may begin" against this scope
   document.

**Parent contract:** None yet — Phase 3 will require its own normative
design document (analogous to Rev26 for the journal) before implementation
begins. This placeholder exists only to formalize the audit gate sequence.

---

## 1. Purpose

Phase 3 migrates the firmware from the current audited 12-channel
configuration to the 16-channel production target. It also introduces
the I/O expander architecture required for channels 13–16 (4 channels
beyond the ESP32's direct GPIO-drive capability for this hardware design).

Phase 3 is **boundary-separated** from Phase 2 per auditor instruction:
parallel work would make failure attribution difficult (journal bug vs.
relay mapping bug vs. expander bug vs. GPIO bug vs. PWA mapping bug).
For a system that will control 220V mains, this boundary is non-negotiable.

---

## 2. Phase 3 Scope (TBD — to be detailed when Phase 3 is authorized)

When Phase 3 is authorized, this document will be expanded to cover:

### P3-1 — I/O expander architecture selection

> **CRITICAL (auditor correction 2026-08-14):** No specific I/O expander
> device (PCF8575, MCP23017, or other) is committed in this document.
> The selection is a hardware engineering decision that must be justified
> against:
>
> - output state at boot (default HIGH/LOW, fail-safe)
> - failure behavior on bus loss (does expander hold last state, or reset to default?)
> - reset behavior (does expander retain state across ESP32 reset?)
> - address configuration (I2C address conflicts with DS3231 at 0x68)
> - electrical compatibility (3.3V vs 5V logic, sink/source current per channel)
> - relay-driver interface (active-LOW/active-HIGH, opto-isolation, flyback diode)
> - fail-safe requirement (relay must default to OFF when expander is unresponsive)
> - bus fault tolerance (single I2C bus failure → 4 channels unreachable;
>   consider dual-bus or watchdog)
> - watchdog / heartbeat (ESP32 must detect expander failure and fail-safe
>   the affected channels)
>
> **Decision required BEFORE Phase 3 implementation begins.** Engineering
> must propose a candidate device + justification, and auditor must approve
> before code is written.

### P3-2 — `RelayDriver.cpp` migration

- Current: direct GPIO drive via `digitalWrite(RELAY_PINS[i], state)`.
- Target: abstraction layer that routes channels 1–12 to direct GPIO,
  channels 13–16 to I/O expander bus.
- Migration must preserve active-LOW semantics for existing 12 channels.
- New channels must default to OFF (HIGH for active-LOW) on boot, before
  any state restoration from journal.

### P3-3 — `Config.h` update

- `NUM_CHANNELS = 16`
- `RELAY_PINS[16]` — first 12 unchanged, last 4 are virtual (expander-
  internal register addresses, not GPIO pin numbers).
- New constant: `EXPANDER_CHANNEL_OFFSET = 12` (channels 13–16 → expander).

### P3-4 — Boot recovery with expander

- Boot sequence must initialize expander to all-OFF BEFORE journal
  recovery runs (so a corrupted journal doesn't drive relays during boot).
- `BootRecoveryPhase` (from Cycle 8B) must be extended to cover expander
  state reconciliation, not just direct GPIO.

### P3-5 — PWA channel mapping update

- PWA `types.ts` must accept 16 channels.
- PWA dashboard UI must render 16 channel cards (or 12 + 4 with a
  visual separator).
- PWA energy analytics must aggregate 16 channels.
- PWA scheduling must support 16-channel selection.

### P3-6 — MQTT mapping update

- `commandHash` canonical form must include channel IDs 13–16.
- No protocol-breaking changes — the `channelId` field in `JournalRecord`
  is already a byte (0..255), so no schema migration needed.

### P3-7 — Hardware acceptance

- 12 power-loss tests re-run on 16-channel hardware (TEST_PLAN.md re-baselined).
- Brownout tests at expander bus failure (channels 13–16 must fail-safe).
- Watchdog tests (ESP32 detects expander unresponsive → fails-safe affected channels).
- All channels cycled ON/OFF 1000× to verify expander reliability.

---

## 3. Phase 3 Acceptance Criteria (TBD)

Will be defined when Phase 3 is authorized. Minimum expectation:
- Static/code evidence (similar to Phase 2 criterion A)
- Host tests for RelayDriver abstraction
- ESP32 runtime tests on 16-channel hardware
- Reboot/recovery tests including expander bus failure
- Power-loss tests on 16-channel hardware (12 scenarios)
- Integration tests (PWA + MQTT + 16-channel ESP32)
- Ed25519 KAT on actual ESP32 (deferred from Phase 1 known-limitation #4)

---

## 4. Out of Scope for Phase 3

| Item | Reason |
|---|---|
| Precharge circuit | BLOCKED in Rev26 (requires `AUTH_EVIDENCE_AUTHENTICATED` which is UNACHIEVABLE). |
| Sender-auth / MQTT ACL enforcement | BLOCKED in Rev26 (broker-side concern, not firmware-verifiable). |
| 32-channel or higher migration | Out of scope; would require multiple I/O expanders + bus topology design. |
| PWA complete rewrite | Out of scope; PWA only gets channel-mapping update, not architectural change. |
| MQTT broker migration | Out of scope; broker-side concerns are operational, not firmware. |

---

## 5. Audit Gate Sequence (reminder)

```
PHASE 1 → AUDIT GATE → PHASE 2 → AUDIT GATE → PHASE 3 (this doc)
    → AUDIT GATE → HARDWARE ACCEPTANCE → 220V PRODUCTION REVIEW
```

Phase 3 will not begin until the Phase 2 audit gate is passed.

---

## 6. Auditor Principle (carried forward)

> Approval is granted only after artefacts themselves demonstrate that
> requirements are met — not after engineering states that work "has been
> done".

This applies to Phase 3 the same way it applies to Phase 1 and Phase 2.
