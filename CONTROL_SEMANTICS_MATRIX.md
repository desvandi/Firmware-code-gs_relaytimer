# CONTROL_SEMANTICS_MATRIX

**Per ChatGPT audit Phase B:** Matrix defines priority, source, safety veto, interlock veto, lockout behavior, stale command behavior, duplicate command behavior, final state for every command/source combination.

---

## 1. Command Source Priority (audit §8)

| Source | Priority | Notes |
|---|---|---|
| SAFETY | 1000 | Always wins — maxOnTime FORCE OFF, interlock, fault |
| EMERGENCY/INTERLOCK | 900 | Interlock emergency override |
| MANUAL_AUTHORIZED | 800 | Operator manual command (authorized) |
| MAINTENANCE | 700 | Maintenance mode override |
| REMOTE_AUTOMATION | 600 | PWA/GAS rule |
| SCHEDULE | 500 | RTC-based schedule |
| PIR | 400 | PIR motion override |
| DEFAULT | 100 | Default OFF |

---

## 2. Decision Table — Current × Request × Source × Safety × Interlock → Result

| # | Current | Request | Source | Safety | Interlock | Result | Notes |
|---|---|---|---|---|---|---|---|
| 1 | OFF | ON | Manual | OK | OK | ON | Normal manual ON |
| 2 | ON | OFF | Manual | OK | OK | OFF | Normal manual OFF |
| 3 | OFF | ON | Schedule | OK | OK | ON | Schedule active |
| 4 | ON | OFF | Schedule | OK | OK | OFF | Schedule ended |
| 5 | OFF | ON | PIR | OK | OK | ON | PIR motion |
| 6 | OFF | ON | Manual | LOCKOUT | OK | OFF + REJECT | Safety lockout (D-007: TRIPPED state) |
| 7 | ON | ON | Manual | LOCKOUT | OK | OFF + REJECT | Cannot re-enable while TRIPPED/ACKNOWLEDGED |
| 8 | OFF | ON | Manual | CLEARED | OK | ON | After explicit clear (D-007: CLEARED state) |
| 9 | OFF | ON | Schedule | OK | BLOCKED | OFF + VETO | Interlock mutual exclusion |
| 10 | A ON | B ON | Schedule | OK | conflict | B rejected | Interlock: A active in group |
| 11 | A OFF | B ON | Schedule | OK | dead-time | DELAYED | Dead time not elapsed |
| 12 | OFF | ON | Manual | OK | OK + chatter | OFF + INHIBIT | Anti-chatter interval |
| 13 | ON | OFF | Manual | OK | OK + minOn | ON + INHIBIT | minOnTime not elapsed |
| 14 | OFF | ON | Manual | OK | OK + minOff | OFF + INHIBIT | minOffTime not elapsed |
| 15 | ON | (maxOnTime) | SAFETY | TRIPPED | OK | FORCE OFF | maxOnTime exceeded (D-007) |
| 16 | OFF | (any) | SAFETY | TRIPPED | OK | OFF | Safety maintains OFF |
| 17 | ON | (any) | SAFETY | TRIPPED | OK | OFF | Safety overrides all |
| 18 | ANY | ON | Unknown | — | — | REJECT | Unknown source rejected (D-008 whitelist) |
| 19 | OFF | ON(seq=100) | Manual | OK | OK | REJECT | Stale command (D-004: seq < lastApplied) |
| 20 | OFF | ON(reqId=dup) | Manual | OK | OK | REPLAY ACK | Duplicate requestId — no re-execute |

---

## 3. Safety Lockout State Machine (D-007)

```
NORMAL
  │
  ↓ (maxOnTime exceeded)
TRIPPED ←─────── (maxOnTimeForced=true, relay FORCED OFF)
  │
  ↓ (operator calls acknowledgeSafetyAlarm)
ACKNOWLEDGED ←── (lockout STILL ACTIVE, relay still OFF)
  │                 ACK ≠ permission!
  ↓ (operator calls clearSafetyLockout)
CLEARED ←────── (maxOnTimeForced=false, relay still OFF but can re-enable)
  │
  ↓ (next tick — armForNormalOperation)
ARMED
  │
  ↓ (next tick — no new trip)
NORMAL
```

**Per ChatGPT audit:** "Acknowledgement = operator mengetahui alarm. Bukan: acknowledgement = sistem otomatis boleh menghidupkan relay."

State | maxOnTimeForced | Relay | Can re-enable?
---|---|---|---
NORMAL | false | controllable | ✅
TRIPPED | true | FORCED OFF | ❌
ACKNOWLEDGED | true | FORCED OFF | ❌
CLEARED | false | OFF | ✅ (next manual command can re-enable)
ARMED | false | controllable | ✅

---

## 4. Command Semantics (D-008 — whitelist architecture)

### Current (v4.3.1): Blacklist (functionally safe, less defensive)

```cpp
if (req.semantics == CommandSemantics::NonIdempotentAction) {
  // REJECT — non-idempotent actions not supported through transaction path
}
// Otherwise: execute
```

### Target (v4.3.2): Explicit whitelist

```cpp
enum class SupportedCommandType : uint8_t {
  SetRelayState,    // IDEMPOTENT_STATE — ON/OFF
  SetMode,          // IDEMPOTENT_STATE — auto/manual
  AcknowledgeAlarm, // IDEMPOTENT_STATE — safety ACK
  ClearSafetyLockout, // IDEMPOTENT_STATE — safety CLEAR
  // PULSE, TOGGLE, START_MOTOR, TRIGGER_CONTACTOR, RESET → NOT in whitelist → REJECT
};

switch (req.commandType) {
  case SupportedCommandType::SetRelayState: /* execute */ break;
  case SupportedCommandType::SetMode: /* execute */ break;
  case SupportedCommandType::AcknowledgeAlarm: /* execute */ break;
  case SupportedCommandType::ClearSafetyLockout: /* execute */ break;
  default: // REJECT — unknown command type
}
```

---

## 5. Command Ordering (D-004 — P1-019, deferred to v4.3.2)

### Current (v4.3.1): requestId dedup only

```
Command A: ON (requestId=abc, seq=100)
Command B: OFF (requestId=def, seq=101)

Network reorders: B arrives first → OFF executed
                 A arrives later → ON executed (WRONG — operator wanted OFF)
```

### Target (v4.3.2): Monotonic command sequence

```
Command A: ON (requestId=abc, seq=100)
Command B: OFF (requestId=def, seq=101)

B arrives first → OFF executed (seq=101, lastApplied=101)
A arrives later → REJECT (seq=100 < lastApplied=101 — STALE)
```

**requestId** answers: "Apakah command yang sama pernah diproses?" (duplicate detection)
**commandSequence** answers: "Apakah command ini masih relevan?" (stale detection)

---

## 6. PWA Timeout Reconciliation (D-003 — P1-018, architecture defined)

```
PWA sends ON command (requestId=abc)
  ↓
Wait 5s for ACK
  ↓
No ACK received → state = TIMEOUT
  ↓
(unknown execution status — device may have executed, may not have)
  ↓
MQTT reconnects
  ↓
PWA fetches /api/status
  ↓
Check: does reportedState match desiredState?
  ↓
YES → state = RESOLVED (CONFIRMED_ON)
NO  → state = STATE_DRIFT (operator must investigate)
```

**Per ChatGPT audit:** "Karena timeout berarti: 'kita tidak tahu apakah command telah dieksekusi'. Bukan: 'command pasti gagal'."

---

## 7. Interlock Application Scope (Phase E)

InterlockEngine MUST apply to ALL command sources:

| Source | Currently applies? | Notes |
|---|---|---|
| Manual | ✅ | Via RelayEngine::tick() → CommandArbiter → InterlockEngine |
| Schedule | ✅ | Same path |
| PIR | ✅ | Same path |
| MQTT | ✅ | Via REST/MQTT handler → setManual → tick → Arbiter → Interlock |
| Automation | ✅ | Same path |
| Boot recovery | ✅ | Boot policy → applyChannelState → recordTransition |
| Safety recovery | ✅ | checkMaxOnTimeExceeded → applyChannelState |
| Test-load (ResistanceEstimator) | ✅ (D-001 fix) | Now via forceChannelState → applyChannelState |

**Result:** Interlock applies to all sources after D-001 fix.

---

## 8. Health Supervisor Recovery Policy (D-009 — architecture defined)

| HealthState | Detection | Action | Recovery |
|---|---|---|---|
| HEALTHY | No alarms | None | N/A |
| WARNING | Active WARNING alarm (low heap, watchdog) | Publish status | Auto-clear when alarm clears |
| DEGRADED | Sensor UNAVAILABLE/ERROR OR task stalled >30s | Publish status + disable affected automation | Auto-recover when sensor/task recovers |
| FAILED | Boot loop OR RTC invalid OR filesystem/NVS not OK | **TODO: force all channels to BOOT_OFF + inhibit scheduler** | Manual intervention required |
| RECOVERING | After FAILED → recovery initiated | **TODO: controlled re-enable of subsystems** | Transition to HEALTHY |

**TODO items** (implementation deferred — architecture defined, wiring pending):
- FAILED → force BOOT_OFF for all channels
- FAILED → inhibit scheduler (no time-based commands)
- RECOVERING → controlled re-enable (one subsystem at a time)

---

## 9. Boot Loop Action Policy (D-010 — architecture defined)

| Trigger | Detection | Action | Recovery |
|---|---|---|---|
| 3+ boots in 60s | Ring buffer of 8 boot timestamps + `bootsInLast60s` counter | **TODO: enter safe state** | Manual intervention |
| High watchdog count relative to boot count | `bootCount > 3 && watchdogResets >= 3` | **TODO: enter safe state** | Manual intervention |

**Safe state on BOOT_LOOP:**
1. Force all channels to BOOT_OFF (hazardous loads de-energized)
2. Inhibit scheduler (no time-based commands)
3. Retain diagnostic data in NVS (boot timestamps, reset reasons)
4. Raise CRITICAL alarm
5. Wait for operator intervention (no automatic recovery — boot loop indicates systemic failure)

---

## 10. Unknown Command/Source Handling (D-008)

Per ChatGPT audit: "Unknown command/source: REJECT"

Current (v4.3.1): Commands with `CommandSemantics::NonIdempotentAction` are rejected. Unknown sources fall through to `CommandSource::Default` (priority 100).

Target (v4.3.2): Explicit whitelist — any command type NOT in `SupportedCommandType` enum → REJECT with `ERR_CMD_UNKNOWN_ACTION`.
