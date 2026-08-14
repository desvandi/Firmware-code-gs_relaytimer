# Transaction Journal — Power-Loss Test Plan

> **Acceptance criterion**: Tidak ada keadaan recovery di mana sebuah command yang sudah dieksekusi dapat dieksekusi kedua kali karena journal kehilangkan committed record.

## Prerequisites

- ESP32-WROOM-32 dev board
- USB cable (data-capable)
- 12-channel relay module connected
- Arduino IDE 2.3.8 + ESP32 core 3.3.7
- Serial Monitor open at 115200 baud
- MQTT client (PWA or mosquitto_pub) for sending commands
- Ability to cut power to ESP32 instantly (USB switch or pull cable)

## Build Configuration

Before flashing, set in `Config.h`:
```cpp
constexpr const char* MQTT_BROKER_HOST = "<your-broker>";
constexpr uint16_t MQTT_BROKER_PORT = 8883;  // TLS
constexpr const char* MQTT_BROKER_USERNAME = "<device-user>";
constexpr const char* MQTT_BROKER_PASSWORD = "<device-pass>";
constexpr const char* MQTT_ROOT_CA = "<PEM>";
constexpr const char* OTA_ED25519_PUBLIC_KEY_HEX = "<64-hex-chars>";
constexpr const char* OTA_HTTPS_ROOT_CA = "<PEM>";
constexpr const char* ALLOWED_CORS_ORIGINS = "https://your-pwa.vercel.app";
```

Compile with `-DPRODUCTION_BUILD` flag (Arduino IDE: Tools → Build Flags).

---

## Test 1: Reboot After Commit Success

**Goal**: Verify committed transaction survives reboot.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send relay ON command with requestId=ABC via MQTT | ACK received, relay ON |
| 2 | Wait 2s (ensure journal write complete) | Serial: `[Journal] Stored: rid=ABC` |
| 3 | Reboot ESP32 (EN button or power cycle) | Serial: `[Journal] Loaded 1 valid transactions` |
| 4 | Send SAME relay ON command with requestId=ABC | ACK received (replayed from journal), relay does NOT toggle |
| 5 | Check Serial | `[MQTT] Duplicate command detected: ABC — queuing original ACK` |

**PASS criteria**: Step 4 — ESP32 does NOT re-execute, replays original ACK from journal.

---

## Test 2: Power Loss During Phase 1 (Blob Write)

**Goal**: Verify partial blob write is rejected on boot.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send relay ON command with requestId=DEF | ACK received, relay ON |
| 2 | Wait for journal store to complete | Serial: `[Journal] Stored: rid=DEF` |
| 3 | Send another relay ON with requestId=GHI | — |
| 4 | **CUT POWER immediately** when Serial shows `[Journal] Phase 1` but BEFORE `Phase 2` | ESP32 dies mid-blob-write |
| 5 | Power on ESP32 | Boot sequence starts |
| 6 | Check Serial on boot | `[Journal] Entry N: not committed (commit=0)` or `INVALID` |
| 7 | Send relay ON with requestId=GHI again | ACK received, relay executes (journal miss = new command) |

**PASS criteria**: Step 6 — uncommitted entry is REJECTED. Step 7 — GHI executes as new command (not blocked by partial entry).

**Note**: Timing this precisely is difficult. Alternative: add `delay(5000)` between Phase 1 and Phase 2 in code temporarily for testing.

---

## Test 3: Power Loss During Phase 1b (writeIdx Persist)

**Goal**: Verify entry stays uncommitted if writeIdx persist fails.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send relay ON with requestId=JKL | ACK received |
| 2 | **CUT POWER** when Serial shows Phase 1 complete but BEFORE Phase 1b (writeIdx) | ESP32 dies |
| 3 | Power on | Boot starts |
| 4 | Check Serial | Entry JKL: `not committed (commit=0)` — rejected |
| 5 | Send relay ON with requestId=JKL again | Executes as new command |

**PASS criteria**: Step 4 — JKL not found in journal (uncommitted). Step 5 — JLI executes fresh.

---

## Test 4: Power Loss During Phase 2 (Commit Flag)

**Goal**: Verify writeIdx is advanced but entry is uncommitted.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Note current writeIdx from Serial (e.g., slot 5) | — |
| 2 | Send relay ON with requestId=MNO | ACK received |
| 3 | **CUT POWER** when Serial shows Phase 1b complete but BEFORE Phase 2 | ESP32 dies |
| 4 | Power on | Boot starts |
| 5 | Check Serial | writeIdx = 6 (advanced), but entry slot 5: `not committed` |
| 6 | Send relay ON with requestId=MNO again | Executes as new command (written to slot 6, NOT slot 5) |

**PASS criteria**: Step 5 — writeIdx advanced (slot 5 skipped, slot 6 is next). Step 6 — MNO executes fresh, written to slot 6. Slot 5 is "wasted" but no committed data lost.

---

## Test 5: Power Loss After Commit (Full Success Path)

**Goal**: Verify committed entry survives immediate power loss.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send relay ON with requestId=PQR | ACK received |
| 2 | **CUT POWER immediately** after Serial shows `Phase 2 commit` success | ESP32 dies |
| 3 | Wait 2s, power on | Boot starts |
| 4 | Check Serial | `[Journal] Loaded 1 valid transactions` — PQR found |
| 5 | Send relay ON with requestId=PQR | ACK replayed from journal, NO re-execution |

**PASS criteria**: Step 4 — PQR is in journal. Step 5 — replay, not re-execute.

---

## Test 6: CRC Corruption Detection

**Goal**: Verify CRC mismatch causes entry rejection.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send relay ON with requestId=STU | ACK received, stored in journal |
| 2 | Note slot number from Serial (e.g., slot 3) | — |
| 3 | Reboot ESP32 | — |
| 4 | Using ESP32 NVS editor (or custom sketch), corrupt 1 byte in `tj_entry_3` blob | CRC no longer matches |
| 5 | Reboot ESP32 | Boot starts |
| 6 | Check Serial | `[Journal] Entry 3: CRC mismatch — CORRUPT` |
| 7 | Send relay ON with requestId=STU | Executes as new command (journal miss) |

**PASS criteria**: Step 6 — CRC mismatch detected, entry rejected. Step 7 — STU re-executes (acceptable — data was corrupted).

**Alternative without NVS editor**: Write a small test sketch that calls `prefs.putBytes("tj_entry_3", corruptedData, len)` to corrupt the blob.

---

## Test 7: NVS Capacity / Blob Size

**Goal**: Verify large ACK JSON (schedule/config) fits in blob.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send schedule upsert with long channel name (32 chars) | ACK received |
| 2 | Check Serial | `[Journal] Stored: rid=... (slot N)` — no size error |
| 3 | Reboot | Entry loaded successfully |
| 4 | Send same schedule upsert with same requestId | Replayed from journal |

**PASS criteria**: No `[Journal] ERROR: blob too large` in Serial. Entry survives reboot.

---

## Test 8: LRU Eviction (64 Entries)

**Goal**: Verify 65th command evicts oldest, but recent entries survive.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send 64 relay commands with requestId=CMD001..CMD064 | All ACKed, all stored |
| 2 | Check Serial | `[Journal] size 64/64` |
| 3 | Send 65th command with requestId=CMD065 | Stored, slot 0 evicted (CMD001 lost) |
| 4 | Reboot | 64 entries loaded (CMD002..CMD065) |
| 5 | Retry CMD002 (evicted) | Re-executes (journal miss — acceptable per documentation) |
| 6 | Retry CMD065 (recent) | Replayed from journal (no re-execute) |

**PASS criteria**: Step 5 — CMD002 re-executes (documented limitation). Step 6 — CMD065 replays (within journal window).

---

## Test 9: ACK Retry Queue After Reboot

**Goal**: Verify pending ACKs are re-queued on boot.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Disconnect PWA / MQTT broker | ESP32 can't publish ACK |
| 2 | Send relay ON with requestId=VWX via broker (from another client) | ESP32 executes, ACK publish fails |
| 3 | Check Serial | `[Journal] ACK published + stored` but `[MQTT ACK] WARNING: publish failed` |
| 4 | Reboot ESP32 (with broker still disconnected) | Boot starts |
| 5 | Check Serial | `[Journal] Queued 1 ACKs for re-delivery after reboot` |
| 6 | Reconnect broker | — |
| 7 | Wait 2s | Serial: `[Journal] ACK delivered: rid=VWX` |
| 8 | PWA receives ACK | — |

**PASS criteria**: Step 5 — ACK re-queued from NVS journal. Step 7 — ACK delivered after broker reconnects.

---

## Test 10: requestId Reuse with Different Command

**Goal**: Verify security violation detection.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Send relay ON CH1 with requestId=YZA | ACK received, CH1 ON |
| 2 | Send relay ON CH8 with SAME requestId=YZA | REJECTED |
| 3 | Check Serial | `SECURITY: requestId reuse with different command: YZA` |
| 4 | Check activity log | AuthFail entry logged |

**PASS criteria**: Step 2 — different command with same requestId is REJECTED, not executed.

---

## Test 11: Ed25519 Known-Answer Test (OTA)

**Goal**: Verify Ed25519 signature verification works on actual ESP32.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Generate keypair: `python3 scripts/sign_firmware.py --gen-keys` | Private + public key created |
| 2 | Paste public key into `Config.h OTA_ED25519_PUBLIC_KEY_HEX` | — |
| 3 | Compile + flash firmware | No compile errors |
| 4 | Sign firmware: `python3 scripts/sign_firmware.py firmware.bin 4.1.0` | .sha256, .sig, .ota.json created |
| 5 | Host firmware.bin on HTTPS server | — |
| 6 | Send OTA command via MQTT with valid signature | ACCEPTED, firmware installs, ESP32 reboots |
| 7 | Send OTA with TAMPERED binary (flip 1 byte) | REJECTED: `SHA-256 mismatch` |
| 8 | Send OTA with MODIFIED signature (flip 1 hex char) | REJECTED: `Ed25519 signature verification FAILED` |
| 9 | Send OTA with WRONG public key in Config.h | REJECTED: `Ed25519 verification FAILED` |
| 10 | Send OTA with OLD version (4.0.0 when current is 4.0.0) | REJECTED: `downgrade blocked` |
| 11 | Send OTA with version "4.1.0foo" | REJECTED: `invalid version format` |

**PASS criteria**: Steps 6-11 all produce expected results. Valid signature accepted, all invalid cases rejected.

---

## Test 12: OTA Boot Health Check + Rollback

**Goal**: Verify firmware rollback after failed boot.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Flash firmware v4.0.0 (healthy) | Boots OK, `markBootHealthy()` called |
| 2 | Send OTA with v4.1.0 that has `while(1){}` in setup() (crash firmware) | OTA installs, ESP32 reboots to v4.1.0 |
| 3 | v4.1.0 boots, setup() hangs in while(1) | Watchdog triggers reboot |
| 4 | Repeat 3 times (boot_attempts = 3) | — |
| 5 | 4th boot attempt | `esp_ota_mark_app_invalid_rollback_and_restart()` called |
| 6 | ESP32 reboots to v4.0.0 (previous partition) | Healthy firmware restored |

**PASS criteria**: Step 6 — ESP32 automatically rolls back to v4.0.0 after 3 failed boots.

---

## Summary Acceptance Matrix

| Test | Acceptance Criterion | Status |
|------|---------------------|--------|
| 1 | Committed transaction survives reboot | ⏳ Pending hardware test |
| 2 | Partial blob write rejected | ⏳ Pending hardware test |
| 3 | writeIdx failure leaves entry uncommitted | ⏳ Pending hardware test |
| 4 | Commit failure advances writeIdx safely | ⏳ Pending hardware test |
| 5 | Full success path durable | ⏳ Pending hardware test |
| 6 | CRC corruption detected | ⏳ Pending hardware test |
| 7 | Large ACK JSON fits in blob | ⏳ Pending hardware test |
| 8 | LRU eviction works as documented | ⏳ Pending hardware test |
| 9 | ACK retry queue persists across reboot | ⏳ Pending hardware test |
| 10 | requestId reuse rejected | ⏳ Pending hardware test |
| 11 | Ed25519 KAT (valid/invalid/tampered) | ⏳ Pending hardware test |
| 12 | OTA rollback after failed boot | ⏳ Pending hardware test |

**All 12 tests must PASS before 220V production deployment.**

---

## How to Execute

1. Flash firmware with `PRODUCTION_BUILD` flag
2. Open Serial Monitor at 115200 baud
3. Execute tests in order (Test 1 → Test 12)
4. Record Serial output for each test
5. Mark PASS/FAIL for each test
6. If any test FAILS, report the Serial output + failure scenario

## Important Notes

- Tests 2-5 require precise timing for power-loss. Add temporary `delay(5000)` between phases in `_saveEntryToNVSAtomic()` for easier testing. Remove before production.
- Test 6 requires NVS corruption. Use a separate sketch or `esptool.py` to write raw NVS partition.
- Test 12 requires a deliberately broken firmware binary. Compile with `while(1){esp_task_wdt_reset();}` in setup() to simulate crash.
- All tests should be repeated 3× to rule out flaky results.
