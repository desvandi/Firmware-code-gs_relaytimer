#!/usr/bin/env python3
"""
test_telemetry_spool_persistence.py — Python simulation of TelemetrySpool
NVS persistence contract.

Validates:
  1. Append 3 critical events + reboot + recover (count + payload content)
  2. Corrupted CRC → record skipped + compaction (count reduced to valid only)
  3. Power loss during commit → previous state intact (MockNVS)
  4. Wrap-around (>CRITICAL_SPOOL_CAPACITY → DROP_OLDEST, count capped)
  5. Duplicate sequence on regular spool → rejected
  6. Successful replay → record removed from pending
  7. Schema version mismatch → NVS cleared

Mirrors firmware TelemetrySpool.cpp behaviour.
"""
from __future__ import annotations

import struct
import sys
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Constants (mirror firmware TelemetrySpool.h)
# ---------------------------------------------------------------------------

CRITICAL_SPOOL_CAPACITY = 8
SPOOL_CAPACITY = 16
MAX_PAYLOAD_LEN = 512
SPOOL_SCHEMA_VERSION = 1
NVS_NAMESPACE = "t12_spool"
NVS_KEY_VERSION = "ver"
NVS_KEY_CRIT_HEAD = "crit_head"
NVS_KEY_CRIT_COUNT = "crit_count"

# struct format: <IIHBH = little-endian
#   I sequence (4)
#   I timestamp (4)
#   H payloadLen (2)
#   B recordType (1)
#   H crc (2)
# Total: 13 bytes header
RECORD_HEADER_FMT = "<IIHBH"
RECORD_HEADER_SIZE = struct.calcsize(RECORD_HEADER_FMT)


# ---------------------------------------------------------------------------
# CRC-16/CCITT (mirror firmware crc16())
# ---------------------------------------------------------------------------

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# TelemetryRecord (mirrors firmware struct)
# ---------------------------------------------------------------------------

class TelemetryRecord:
    __slots__ = ("sequence", "timestamp", "payloadLen", "recordType", "crc",
                 "payload")

    def __init__(self):
        self.sequence: int = 0
        self.timestamp: int = 0
        self.payloadLen: int = 0
        self.recordType: int = 0
        self.crc: int = 0
        self.payload: bytes = b""

    def serialize(self) -> bytes:
        return struct.pack(RECORD_HEADER_FMT, self.sequence, self.timestamp,
                            self.payloadLen, self.recordType, self.crc) + self.payload

    @classmethod
    def deserialize(cls, data: bytes) -> "TelemetryRecord":
        r = cls()
        if len(data) < RECORD_HEADER_SIZE:
            return r
        (r.sequence, r.timestamp, r.payloadLen, r.recordType, r.crc) = \
            struct.unpack(RECORD_HEADER_FMT, data[:RECORD_HEADER_SIZE])
        r.payload = data[RECORD_HEADER_SIZE:RECORD_HEADER_SIZE + r.payloadLen]
        return r

    def is_empty(self) -> bool:
        return self.sequence == 0 and self.payloadLen == 0

    def compute_crc(self) -> int:
        buf = bytearray()
        buf += struct.pack("<I", self.sequence)
        buf += struct.pack("<I", self.timestamp)
        buf += struct.pack("<H", self.payloadLen)
        buf += struct.pack("<B", self.recordType)
        if 0 < self.payloadLen <= MAX_PAYLOAD_LEN:
            buf += self.payload[:self.payloadLen]
        return crc16(bytes(buf))

    def verify(self) -> bool:
        return self.crc == self.compute_crc()


def make_record(seq: int, ts: int, payload: bytes, record_type: int = 0) -> TelemetryRecord:
    r = TelemetryRecord()
    r.sequence = seq
    r.timestamp = ts
    r.payload = payload[:MAX_PAYLOAD_LEN]
    r.payloadLen = len(r.payload)
    r.recordType = record_type
    r.crc = r.compute_crc()
    return r


# ---------------------------------------------------------------------------
# MockNVS — atomic-ish begin/end + power-loss simulation
# ---------------------------------------------------------------------------

class MockNVS:
    def __init__(self):
        self._store: Dict[str, bytes] = {}
        self._in_txn: bool = False
        self._txn_buf: Dict[str, bytes] = {}
        self._power_loss_during_commit: bool = False

    # Atomic begin/end (mirror Preferences::begin/end)
    def begin(self, namespace: str, readonly: bool = False) -> bool:
        self._in_txn = True
        self._txn_buf = dict(self._store)
        return True

    def end(self) -> None:
        if self._in_txn:
            if self._power_loss_during_commit:
                # Power loss: do NOT commit pending writes (transaction aborted)
                self._power_loss_during_commit = False
                self._in_txn = False
                self._txn_buf = {}
                return
            self._store = dict(self._txn_buf)
        self._in_txn = False
        self._txn_buf = {}

    def put_bytes(self, key: str, data: bytes) -> bool:
        if not self._in_txn:
            return False
        self._txn_buf[key] = data
        return True

    def get_bytes(self, key: str) -> Optional[bytes]:
        store = self._txn_buf if self._in_txn else self._store
        return store.get(key)

    def get_bytes_length(self, key: str) -> int:
        store = self._txn_buf if self._in_txn else self._store
        v = store.get(key)
        return len(v) if v is not None else 0

    def put_uchar(self, key: str, val: int) -> bool:
        return self.put_bytes(key, bytes([val & 0xFF]))

    def get_uchar(self, key: str, default: int = 0) -> int:
        v = self.get_bytes(key)
        if v is None or len(v) == 0:
            return default
        return v[0]

    def clear(self) -> None:
        if self._in_txn:
            self._txn_buf = {}
        else:
            self._store = {}

    def simulate_power_loss_during_commit(self) -> None:
        """Next end() call will NOT commit pending writes."""
        self._power_loss_during_commit = True


# ---------------------------------------------------------------------------
# TelemetrySpoolSim — mirrors firmware TelemetrySpool
# ---------------------------------------------------------------------------

class TelemetrySpoolSim:
    def __init__(self, nvs: MockNVS):
        self._nvs = nvs
        self._records: List[TelemetryRecord] = [TelemetryRecord() for _ in range(SPOOL_CAPACITY)]
        self._criticalRecords: List[TelemetryRecord] = [TelemetryRecord() for _ in range(CRITICAL_SPOOL_CAPACITY)]
        self._head: int = 0
        self._count: int = 0
        self._criticalHead: int = 0
        self._criticalCount: int = 0
        self._dropCount: int = 0
        self._replayCount: int = 0
        self._lastSpooledSeq: int = 0
        self._nvsLoaded: bool = False

    def begin(self) -> None:
        self._head = 0
        self._count = 0
        self._dropCount = 0
        self._replayCount = 0
        self._lastSpooledSeq = 0
        self._criticalHead = 0
        self._criticalCount = 0
        self._nvsLoaded = False
        self._load_critical_from_nvs()

    def _nvs_key_for_index(self, idx: int) -> str:
        return f"rec_{idx}"

    def _load_critical_from_nvs(self) -> None:
        if not self._nvs.begin(NVS_NAMESPACE, readonly=True):
            return
        stored_ver = self._nvs.get_uchar(NVS_KEY_VERSION, 0)
        if stored_ver != SPOOL_SCHEMA_VERSION:
            # Schema mismatch → clear NVS
            self._nvs.end()
            self._clear_nvs_spool()
            if self._nvs.begin(NVS_NAMESPACE, readonly=False):
                self._nvs.put_uchar(NVS_KEY_VERSION, SPOOL_SCHEMA_VERSION)
                self._nvs.end()
            self._nvsLoaded = True
            return

        self._criticalHead = self._nvs.get_uchar(NVS_KEY_CRIT_HEAD, 0)
        self._criticalCount = self._nvs.get_uchar(NVS_KEY_CRIT_COUNT, 0)
        if self._criticalCount > CRITICAL_SPOOL_CAPACITY:
            self._nvs.end()
            self._clear_nvs_spool()
            self._criticalCount = 0
            self._criticalHead = 0
            self._nvsLoaded = True
            return

        validCount = 0
        firstInvalid = 0xFF
        for i in range(CRITICAL_SPOOL_CAPACITY):
            key = self._nvs_key_for_index(i)
            needed = self._nvs.get_bytes_length(key)
            if needed != RECORD_HEADER_SIZE + MAX_PAYLOAD_LEN:
                continue
            data = self._nvs.get_bytes(key)
            if data is None or len(data) != RECORD_HEADER_SIZE + MAX_PAYLOAD_LEN:
                continue
            tmp = TelemetryRecord.deserialize(data)
            # Empty-slot detection
            if tmp.sequence == 0 and tmp.payloadLen == 0:
                continue
            if not tmp.verify():
                if firstInvalid == 0xFF:
                    firstInvalid = i
                continue
            self._criticalRecords[i] = tmp
            validCount += 1

        if firstInvalid != 0xFF and validCount < self._criticalCount:
            # Compaction
            compacted = [TelemetryRecord() for _ in range(CRITICAL_SPOOL_CAPACITY)]
            ci = 0
            for i in range(CRITICAL_SPOOL_CAPACITY):
                if self._criticalRecords[i].verify():
                    compacted[ci] = self._criticalRecords[i]
                    ci += 1
            self._criticalRecords = compacted
            self._criticalHead = ci % CRITICAL_SPOOL_CAPACITY
            self._criticalCount = ci
            self._nvs.end()
            self._persist_critical_to_nvs()
        else:
            self._nvs.end()

        self._nvsLoaded = True

    def _persist_critical_to_nvs(self) -> None:
        if not self._nvs.begin(NVS_NAMESPACE, readonly=False):
            return
        self._nvs.put_uchar(NVS_KEY_VERSION, SPOOL_SCHEMA_VERSION)
        self._nvs.put_uchar(NVS_KEY_CRIT_HEAD, self._criticalHead)
        self._nvs.put_uchar(NVS_KEY_CRIT_COUNT, self._criticalCount)
        for i in range(CRITICAL_SPOOL_CAPACITY):
            key = self._nvs_key_for_index(i)
            # Always write full fixed-size slot (header + MAX_PAYLOAD_LEN)
            data = self._criticalRecords[i].serialize()
            # Pad to fixed slot size
            data = data + b"\x00" * (RECORD_HEADER_SIZE + MAX_PAYLOAD_LEN - len(data))
            self._nvs.put_bytes(key, data)
        self._nvs.end()

    def _clear_nvs_spool(self) -> None:
        if not self._nvs.begin(NVS_NAMESPACE, readonly=False):
            return
        self._nvs.clear()
        self._nvs.end()

    def spool(self, sequence: int, timestamp: int, payload: bytes) -> bool:
        if not payload or len(payload) > MAX_PAYLOAD_LEN:
            return False
        if sequence == self._lastSpooledSeq and self._lastSpooledSeq != 0:
            return False
        self._lastSpooledSeq = sequence
        self._records[self._head] = make_record(sequence, timestamp, payload, 0)
        self._head = (self._head + 1) % SPOOL_CAPACITY
        if self._count < SPOOL_CAPACITY:
            self._count += 1
        else:
            self._dropCount += 1
        return True

    def spool_critical(self, sequence: int, timestamp: int, payload: bytes) -> bool:
        if not payload or len(payload) > MAX_PAYLOAD_LEN:
            return False
        rec = make_record(sequence, timestamp, payload, 1)
        self._criticalRecords[self._criticalHead] = rec
        self._criticalHead = (self._criticalHead + 1) % CRITICAL_SPOOL_CAPACITY
        if self._criticalCount < CRITICAL_SPOOL_CAPACITY:
            self._criticalCount += 1
        # DROP_OLDEST: count capped at capacity (head wraps, oldest is overwritten)
        self._persist_critical_to_nvs()
        return True

    def replay_one(self) -> int:
        if self._count == 0 and self._criticalCount == 0:
            return 0
        if self._criticalCount > 0:
            oldest = (self._criticalHead + CRITICAL_SPOOL_CAPACITY - self._criticalCount) % CRITICAL_SPOOL_CAPACITY
            if self._criticalRecords[oldest].verify():
                self._criticalCount -= 1
                self._replayCount += 1
                self._persist_critical_to_nvs()
                return 1
            else:
                self._criticalCount -= 1
                self._persist_critical_to_nvs()
        if self._count > 0:
            oldest = (self._head + SPOOL_CAPACITY - self._count) % SPOOL_CAPACITY
            if self._records[oldest].verify():
                self._count -= 1
                self._replayCount += 1
                return 1
            else:
                self._count -= 1
        return 0

    @property
    def criticalCount(self) -> int:
        return self._criticalCount

    @property
    def count(self) -> int:
        return self._count

    @property
    def dropCount(self) -> int:
        return self._dropCount

    @property
    def replayCount(self) -> int:
        return self._replayCount


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_1_append_reboot_recover() -> Tuple[bool, str]:
    nvs = MockNVS()
    sim = TelemetrySpoolSim(nvs)
    sim.begin()
    sim.spool_critical(1, 1000, b"BOOT")
    sim.spool_critical(2, 2000, b"ALARM:overcurrent")
    sim.spool_critical(3, 3000, b"FAULT:relay_stuck")

    # Simulate reboot: new spool instance reads same NVS
    sim2 = TelemetrySpoolSim(nvs)
    sim2.begin()
    if sim2.criticalCount != 3:
        return False, f"expected criticalCount=3 after reboot, got {sim2.criticalCount}"
    # Verify payload content (in order of insertion)
    payloads = []
    for i in range(sim2.criticalCount):
        oldest = (sim2._criticalHead + CRITICAL_SPOOL_CAPACITY - sim2.criticalCount + i) % CRITICAL_SPOOL_CAPACITY
        rec = sim2._criticalRecords[oldest]
        payloads.append(rec.payload.decode("utf-8", errors="replace"))
    if payloads != ["BOOT", "ALARM:overcurrent", "FAULT:relay_stuck"]:
        return False, f"payload mismatch after reboot: {payloads}"
    return True, "3 critical events recovered after reboot with correct payloads"


def test_2_corrupted_crc_skipped_compacted() -> Tuple[bool, str]:
    nvs = MockNVS()
    sim = TelemetrySpoolSim(nvs)
    sim.begin()
    sim.spool_critical(1, 1000, b"A")
    sim.spool_critical(2, 2000, b"B")
    sim.spool_critical(3, 3000, b"C")
    assert sim.criticalCount == 3

    # Corrupt record 1's CRC directly in NVS
    if not nvs.begin(NVS_NAMESPACE, readonly=False):
        return False, "NVS begin failed"
    data = nvs.get_bytes("rec_1")
    if data is None:
        return False, "rec_1 not found in NVS"
    # CRC is at offset (8 + 4 + 4 + 2 + 1) = 19 in the slot, last 2 bytes before payload
    # Actually: struct is <IIHBH = 4+4+2+1+2 = 13 bytes header, then payload (padded to 512)
    # CRC is at offset 11..12 (H is 2 bytes after I,I,H,B)
    corrupted = bytearray(data)
    corrupted[11] ^= 0xFF  # flip a bit in CRC
    nvs.put_bytes("rec_1", bytes(corrupted))
    nvs.end()

    sim2 = TelemetrySpoolSim(nvs)
    sim2.begin()
    if sim2.criticalCount != 2:
        return False, f"expected criticalCount=2 after compaction, got {sim2.criticalCount}"
    return True, "corrupted CRC skipped + compaction reduced count to 2"


def test_3_power_loss_during_commit() -> Tuple[bool, str]:
    nvs = MockNVS()
    sim = TelemetrySpoolSim(nvs)
    sim.begin()
    sim.spool_critical(1, 1000, b"before")
    sim.spool_critical(2, 2000, b"also-before")
    assert sim.criticalCount == 2

    # Now attempt to spool another critical event but simulate power loss
    # during the commit (end() will not flush).
    nvs.simulate_power_loss_during_commit()
    # spool_critical calls _persist_critical_to_nvs which calls begin/end
    # We need to ensure the spool itself doesn't update its in-memory count
    # Actually, in firmware, the in-memory state is updated BEFORE the persist.
    # On reboot, the in-memory state is rebuilt from NVS — so if NVS commit
    # was lost, the post-reboot state should reflect the OLD NVS state.
    # We need to simulate this more carefully.
    # Persist with power loss — the new record won't reach NVS.
    sim.spool_critical(3, 3000, b"lost-on-power-fail")
    # In-memory sim has 3, but NVS still has 2 (power loss rolled back the txn)

    # Reboot: new sim reads NVS — should see only 2
    sim2 = TelemetrySpoolSim(nvs)
    sim2.begin()
    if sim2.criticalCount != 2:
        return False, f"expected criticalCount=2 after power-loss, got {sim2.criticalCount}"
    # The third record's payload should NOT be present
    found_lost = False
    for i in range(CRITICAL_SPOOL_CAPACITY):
        rec = sim2._criticalRecords[i]
        if rec.payload == b"lost-on-power-fail":
            found_lost = True
            break
    if found_lost:
        return False, "power-lost record unexpectedly recovered from NVS"
    return True, "power loss during commit left previous state intact (2 records)"


def test_4_wrap_around_drop_oldest() -> Tuple[bool, str]:
    nvs = MockNVS()
    sim = TelemetrySpoolSim(nvs)
    sim.begin()
    # Spool > CRITICAL_SPOOL_CAPACITY (8) events
    for i in range(1, CRITICAL_SPOOL_CAPACITY + 4):  # 11 events
        sim.spool_critical(i, i * 1000, f"event-{i}".encode())
    if sim.criticalCount != CRITICAL_SPOOL_CAPACITY:
        return False, f"expected count capped at {CRITICAL_SPOOL_CAPACITY}, got {sim.criticalCount}"
    # Oldest events (1, 2, 3) should have been dropped (overwritten)
    payloads_present = set()
    for i in range(CRITICAL_SPOOL_CAPACITY):
        rec = sim._criticalRecords[i]
        if rec.payload:
            payloads_present.add(rec.payload.decode("utf-8", errors="replace"))
    if "event-1" in payloads_present:
        return False, f"event-1 should have been dropped (wrap-around), still present"
    # Last event should be present
    if f"event-{CRITICAL_SPOOL_CAPACITY + 3}" not in payloads_present:
        return False, f"last event not present after wrap-around"
    return True, f"wrap-around dropped oldest; count capped at {CRITICAL_SPOOL_CAPACITY}"


def test_5_duplicate_sequence_regular_spool_rejected() -> Tuple[bool, str]:
    nvs = MockNVS()
    sim = TelemetrySpoolSim(nvs)
    sim.begin()
    ok1 = sim.spool(42, 1000, b"first")
    ok2 = sim.spool(42, 2000, b"second-same-seq")
    if not ok1:
        return False, "first spool should succeed"
    if ok2:
        return False, "duplicate sequence should be rejected on regular spool"
    if sim.count != 1:
        return False, f"count should be 1 after rejected duplicate, got {sim.count}"
    return True, "duplicate sequence on regular spool rejected"


def test_6_successful_replay_removes_record() -> Tuple[bool, str]:
    nvs = MockNVS()
    sim = TelemetrySpoolSim(nvs)
    sim.begin()
    sim.spool_critical(1, 1000, b"replay-me")
    assert sim.criticalCount == 1
    rc = sim.replay_one()
    if rc != 1:
        return False, f"replay_one should return 1, got {rc}"
    if sim.criticalCount != 0:
        return False, f"after replay, criticalCount should be 0, got {sim.criticalCount}"
    if sim.replayCount != 1:
        return False, f"replayCount should be 1, got {sim.replayCount}"
    return True, "successful replay removed record from pending (count → 0)"


def test_7_schema_mismatch_clears_nvs() -> Tuple[bool, str]:
    nvs = MockNVS()
    sim = TelemetrySpoolSim(nvs)
    sim.begin()
    sim.spool_critical(1, 1000, b"old-schema-data")
    # Manually corrupt the schema version
    if not nvs.begin(NVS_NAMESPACE, readonly=False):
        return False, "NVS begin failed"
    nvs.put_uchar(NVS_KEY_VERSION, 99)  # wrong version
    nvs.end()

    sim2 = TelemetrySpoolSim(nvs)
    sim2.begin()
    if sim2.criticalCount != 0:
        return False, f"schema mismatch should clear NVS, got criticalCount={sim2.criticalCount}"
    # Verify NVS schema version was reset to current
    if not nvs.begin(NVS_NAMESPACE, readonly=True):
        return False, "NVS begin (read) failed"
    ver = nvs.get_uchar(NVS_KEY_VERSION, 0)
    nvs.end()
    if ver != SPOOL_SCHEMA_VERSION:
        return False, f"NVS schema version should be reset to {SPOOL_SCHEMA_VERSION}, got {ver}"
    return True, "schema mismatch cleared NVS (count=0, version reset)"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    print("=" * 78)
    print("TelemetrySpool NVS Persistence — 7 Test Matrix")
    print("=" * 78)

    tests = [
        ("1. Append + reboot + recover", test_1_append_reboot_recover),
        ("2. Corrupted CRC → skipped + compaction", test_2_corrupted_crc_skipped_compacted),
        ("3. Power loss during commit → previous state intact", test_3_power_loss_during_commit),
        ("4. Wrap-around DROP_OLDEST + cap", test_4_wrap_around_drop_oldest),
        ("5. Duplicate sequence on regular spool → rejected", test_5_duplicate_sequence_regular_spool_rejected),
        ("6. Successful replay → removed from pending", test_6_successful_replay_removes_record),
        ("7. Schema version mismatch → NVS cleared", test_7_schema_mismatch_clears_nvs),
    ]

    passed = 0
    failed = 0
    for name, fn in tests:
        try:
            ok, msg = fn()
        except Exception as e:
            ok, msg = False, f"EXCEPTION: {e!r}"
        status = "PASS" if ok else "FAIL"
        if ok:
            passed += 1
        else:
            failed += 1
        print(f"\n  [{status}] {name}")
        print(f"           {msg}")

    print()
    print("=" * 78)
    print(f"Results: {passed}/{len(tests)} passed")
    print("=" * 78)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
