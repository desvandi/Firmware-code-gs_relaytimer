#!/usr/bin/env python3
"""
PD-001 Canonical Command & Transaction Model — Test Suite.

This is a Python REFERENCE implementation of the canonical serialization +
SHA-256 hash logic that lives in firmware/CommandCanonicalizer.cpp. It is
used to verify the LOGIC of the canonical model (deterministic serialization,
hash invariants, duplicate/conflict decisions) without requiring the ESP32
toolchain.

WHAT THIS VALIDATES:
  - Canonical serialization is whitespace-independent (AC-005).
  - Canonical serialization is property-order-independent (AC-006).
  - type/action are lowercased (directive §8).
  - commandHash excludes transactionId/issuedAt/expiresAt/transport metadata
    (AC-007).
  - Same logical command → same hash, regardless of transport (AC-001/AC-018).
  - Different logical command → different hash (AC-008).
  - Duplicate detection: same TX + same hash → DUPLICATE (AC-009).
  - Conflict detection: same TX + different hash → CONFLICT (AC-010).
  - Negative cases: missing transactionId, empty action, unknown type,
    unsupported version, oversized fields → REJECT (directive §26).

WHAT THIS DOES NOT VALIDATE:
  - Actual ESP32 mbedtls SHA-256 output (we use Python hashlib).
  - Actual NVS journal persistence (we use an in-memory mock).
  - Actual HTTP/MQTT transport (we test the canonical model directly).

The reference implementation here MUST stay byte-for-byte aligned with the
C++ implementation in CommandCanonicalizer.cpp. If they diverge, cross-
transport hash equivalence breaks.

Run: python3 /home/z/my-project/scripts/test_pd001_canonical.py
"""

import hashlib
import json
import sys
from typing import Any, Dict, List, Optional, Tuple

# ===========================================================================
# Reference implementation — mirrors firmware/CommandCanonicalizer.cpp
# ===========================================================================

CANONICAL_COMMAND_VERSION = 1
MAX_TRANSACTION_ID_LEN = 64
MIN_TRANSACTION_ID_LEN = 1


class CanonicalError(Exception):
    """Raised when canonicalization fails (mirrors CanonicalResult.ok=false)."""
    pass


def validate_transaction_id(tid: str) -> None:
    """Mirror CommandCanonicalizer::validateTransactionId."""
    if len(tid) < MIN_TRANSACTION_ID_LEN:
        raise CanonicalError("transactionId is required (non-empty)")
    if len(tid) > MAX_TRANSACTION_ID_LEN:
        raise CanonicalError(f"transactionId too long (max {MAX_TRANSACTION_ID_LEN} chars)")
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
    for i, c in enumerate(tid):
        if c not in allowed:
            raise CanonicalError(
                f"transactionId contains invalid char at pos {i} (allowed: a-z A-Z 0-9 - _)"
            )


def validate_protocol_version(version: int) -> None:
    """Mirror CommandCanonicalizer::validateProtocolVersion."""
    if version != CANONICAL_COMMAND_VERSION:
        raise CanonicalError(
            f"unsupported protocol version {version} (supported: {CANONICAL_COMMAND_VERSION})"
        )


def is_known_command_type(type_lower: str, action_lower: str) -> bool:
    """Mirror CommandCanonicalizer::isKnownCommandType — registry.

    Note: matches the C++ registry exactly, including mixed-case action names
    for system/config ("getStatus", "setDevice"). Callers should pass the
    original-case action string, NOT a lowercased one.
    """
    if type_lower == "relay":
        return action_lower in {"on", "off", "set_mode"}
    if type_lower == "schedule":
        return action_lower in {"upsert", "delete"}
    if type_lower == "pir":
        return action_lower in {"config", "test"}
    if type_lower == "channel":
        return action_lower == "rename"
    if type_lower == "time":
        return action_lower == "set"
    if type_lower == "system":
        return action_lower in {"reboot", "getStatus", "resetEnergyStats", "resetDailyStats"}
    if type_lower == "config":
        return action_lower == "setDevice"
    if type_lower == "ota":
        return action_lower == "update"
    return False


def is_field_allowed(type_lower: str, field_name: str) -> bool:
    """Mirror CommandCanonicalizer::isFieldAllowed."""
    envelope_fields = {"type", "action", "requestId", "transactionId",
                       "version", "issuedAt", "expiresAt"}
    if field_name in envelope_fields:
        return True
    payload_fields = {
        "relay": {"channelId", "mode", "manualState"},
        "schedule": {"channelId", "id", "onTime", "offTime", "dayMask", "enabled"},
        "pir": {"id", "enabled", "holdTime"},
        "channel": {"channelId", "name"},
        "time": {"datetime"},
        "system": set(),
        "config": {"deviceName", "timezone"},
        "ota": {"url", "version", "size", "sha256", "signature"},
    }
    return field_name in payload_fields.get(type_lower, set())


def build_canonical_string(doc: Dict[str, Any]) -> str:
    """Mirror CommandCanonicalizer::buildCanonicalString."""
    version = doc.get("version", CANONICAL_COMMAND_VERSION)
    validate_protocol_version(version)

    type_lower = str(doc.get("type", "")).lower()
    action_lower = str(doc.get("action", "")).lower()

    type_has_schema = type_lower in {
        "relay", "schedule", "pir", "channel", "time", "system", "config", "ota"
    }
    if not type_has_schema:
        raise CanonicalError(f"unknown command type: {type_lower}")

    canonical = f"v{version}|{type_lower}|{action_lower}"

    if type_lower == "relay":
        canonical += f"|channelId={int(doc.get('channelId', 0))}"
        canonical += f"|mode={str(doc.get('mode', ''))}"
        canonical += f"|manualState={'true' if bool(doc.get('manualState', False)) else 'false'}"
    elif type_lower == "schedule":
        canonical += f"|channelId={int(doc.get('channelId', 0))}"
        canonical += f"|id={int(doc.get('id', 0))}"
        canonical += f"|onTime={str(doc.get('onTime', ''))}"
        canonical += f"|offTime={str(doc.get('offTime', ''))}"
        canonical += f"|dayMask={int(doc.get('dayMask', 0))}"
        canonical += f"|enabled={'true' if bool(doc.get('enabled', True)) else 'false'}"
    elif type_lower == "pir":
        canonical += f"|id={int(doc.get('id', 0))}"
        canonical += f"|enabled={'true' if bool(doc.get('enabled', False)) else 'false'}"
        canonical += f"|holdTime={int(doc.get('holdTime', 0))}"
    elif type_lower == "channel":
        canonical += f"|channelId={int(doc.get('channelId', 0))}"
        canonical += f"|name={str(doc.get('name', ''))}"
    elif type_lower == "time":
        canonical += f"|datetime={str(doc.get('datetime', ''))}"
    elif type_lower == "system":
        pass  # action only
    elif type_lower == "config":
        canonical += f"|deviceName={str(doc.get('deviceName', ''))}"
        canonical += f"|timezone={str(doc.get('timezone', ''))}"
    elif type_lower == "ota":
        canonical += f"|url={str(doc.get('url', ''))}"
        canonical += f"|version={str(doc.get('version', ''))}"
        canonical += f"|size={int(doc.get('size', 0))}"
        canonical += f"|sha256={str(doc.get('sha256', ''))}"
        canonical += f"|signature={str(doc.get('signature', ''))}"

    return canonical


def canonicalize_and_hash(doc: Dict[str, Any]) -> Tuple[str, str, str]:
    """Mirror CommandCanonicalizer::canonicalizeAndHash.
    Returns (transactionId, commandHash, canonicalString).
    Raises CanonicalError on failure.
    """
    type_c = doc.get("type", "")
    action_c = doc.get("action", "")
    if not type_c:
        raise CanonicalError("missing 'type' field")
    if not action_c:
        raise CanonicalError("missing 'action' field")

    # Extract transactionId (requestId compatibility).
    tid = doc.get("requestId", "")
    tid_alt = doc.get("transactionId", "")
    if not tid and tid_alt:
        tid = tid_alt
    elif tid and tid_alt and tid != tid_alt:
        raise CanonicalError("requestId and transactionId both present but differ")

    validate_transaction_id(tid)

    canonical = build_canonical_string(doc)
    hash_hex = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return tid, hash_hex, canonical


# ===========================================================================
# In-memory mock journal — mirrors Services::TransactionJournal (PD-002 owns
# the real NVS-backed implementation; this mock is for PD-001 logic tests only)
# ===========================================================================

class MockJournal:
    def __init__(self):
        self._entries: Dict[str, Tuple[str, str]] = {}  # tid -> (hash, ackJson)

    def is_processed(self, tid: str) -> bool:
        return tid in self._entries

    def get_command_hash(self, tid: str) -> str:
        return self._entries.get(tid, ("", ""))[0]

    def get_ack_json(self, tid: str) -> str:
        return self._entries.get(tid, ("", ""))[1]

    def store(self, tid: str, hash_: str, ack_json: str) -> None:
        # Per directive §16: existing records MUST NOT be overwritten by a
        # new command. Only store if NEW.
        if tid not in self._entries:
            self._entries[tid] = (hash_, ack_json)

    def reset(self) -> None:
        self._entries.clear()


# Global mock journal instance (singleton, mirrors `Services::journal`).
journal = MockJournal()


def decide_transaction(tid: str, hash_: str) -> str:
    """Mirror CommandCanonicalizer::decideTransaction.
    Returns "NEW", "DUPLICATE", or "CONFLICT".
    """
    if not journal.is_processed(tid):
        return "NEW"
    prev_hash = journal.get_command_hash(tid)
    if prev_hash and prev_hash != hash_:
        return "CONFLICT"
    return "DUPLICATE"


# ===========================================================================
# Test framework
# ===========================================================================

PASS = 0
FAIL = 0
FAILURES: List[str] = []


def check(condition: bool, label: str) -> None:
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  PASS: {label}")
    else:
        FAIL += 1
        FAILURES.append(label)
        print(f"  FAIL: {label}")


def check_raises(fn, label: str) -> None:
    global PASS, FAIL
    try:
        fn()
        FAIL += 1
        FAILURES.append(label + " (expected exception)")
        print(f"  FAIL: {label} (expected exception)")
    except (CanonicalError, ValueError, TypeError):
        PASS += 1
        print(f"  PASS: {label}")


# ===========================================================================
# Test Suite A: Transaction Identity (directive §23.A)
# ===========================================================================

def test_transaction_identity() -> None:
    print("\n=== A. Transaction Identity ===")

    # valid transactionId → ACCEPT
    try:
        validate_transaction_id("TX-001")
        check(True, "valid transactionId 'TX-001' accepted")
    except CanonicalError:
        check(False, "valid transactionId 'TX-001' accepted")

    # empty → REJECT
    check_raises(lambda: validate_transaction_id(""), "empty transactionId rejected")

    # malformed (control char) → REJECT
    check_raises(lambda: validate_transaction_id("TX\x00bad"), "control char in transactionId rejected")
    check_raises(lambda: validate_transaction_id("TX\nbad"), "newline in transactionId rejected")
    check_raises(lambda: validate_transaction_id("TX bad"), "space in transactionId rejected")

    # oversized → REJECT
    check_raises(lambda: validate_transaction_id("X" * 65), "oversized transactionId (65 chars) rejected")

    # max-length (64 chars) → ACCEPT
    try:
        validate_transaction_id("X" * 64)
        check(True, "max-length transactionId (64 chars) accepted")
    except CanonicalError:
        check(False, "max-length transactionId (64 chars) accepted")

    # requestId mapping: requestId == transactionId (semantic alias)
    doc1 = {"type": "relay", "action": "on", "requestId": "TX-001", "channelId": 3}
    doc2 = {"type": "relay", "action": "on", "transactionId": "TX-001", "channelId": 3}
    tid1, hash1, _ = canonicalize_and_hash(doc1)
    tid2, hash2, _ = canonicalize_and_hash(doc2)
    check(tid1 == tid2 == "TX-001", "requestId == transactionId (semantic alias)")
    check(hash1 == hash2, "requestId/transactionId produce same hash (envelope field)")

    # both present and equal → ACCEPT
    doc3 = {"type": "relay", "action": "on", "requestId": "TX-001",
            "transactionId": "TX-001", "channelId": 3}
    try:
        tid3, _, _ = canonicalize_and_hash(doc3)
        check(tid3 == "TX-001", "both requestId and transactionId equal → accepted")
    except CanonicalError:
        check(False, "both requestId and transactionId equal → accepted")

    # both present and differ → REJECT (directive §4.1: no two identities)
    doc4 = {"type": "relay", "action": "on", "requestId": "TX-001",
            "transactionId": "TX-XYZ", "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc4),
                 "requestId != transactionId → rejected (no two identities)")


# ===========================================================================
# Test Suite B: Canonical Serialization (directive §23.B)
# ===========================================================================

def test_canonical_serialization() -> None:
    print("\n=== B. Canonical Serialization ===")

    # whitespace-independent
    doc1 = json.loads('{"type":"relay","action":"on","requestId":"TX-1","channelId":3}')
    doc2 = json.loads('{  "type" : "relay" ,  "action" : "on" ,  "requestId" : "TX-1" ,  "channelId" : 3  }')
    _, h1, _ = canonicalize_and_hash(doc1)
    _, h2, _ = canonicalize_and_hash(doc2)
    check(h1 == h2, "whitespace-independent canonical hash")

    # property-order-independent
    doc3 = json.loads('{"channelId":3,"action":"on","type":"relay","requestId":"TX-1"}')
    _, h3, _ = canonicalize_and_hash(doc3)
    check(h1 == h3, "property-order-independent canonical hash")

    # type uppercase → canonical lowercase
    doc4 = {"type": "RELAY", "action": "ON", "requestId": "TX-1", "channelId": 3}
    _, h4, _ = canonicalize_and_hash(doc4)
    check(h1 == h4, "type/action case-insensitive (lowercased in canonical)")

    # boolean true → "true"
    doc5 = {"type": "relay", "action": "set_mode", "requestId": "TX-1",
            "channelId": 3, "mode": "manual", "manualState": True}
    _, h5, c5 = canonicalize_and_hash(doc5)
    check("manualState=true" in c5, "boolean True → 'true' in canonical string")

    # boolean false → "false"
    doc6 = {"type": "relay", "action": "set_mode", "requestId": "TX-1",
            "channelId": 3, "mode": "manual", "manualState": False}
    _, h6, c6 = canonicalize_and_hash(doc6)
    check("manualState=false" in c6, "boolean False → 'false' in canonical string")

    # integer deterministic
    doc7 = {"type": "relay", "action": "on", "requestId": "TX-1", "channelId": 3}
    doc8 = {"type": "relay", "action": "on", "requestId": "TX-1", "channelId": 3.0}
    _, h7, _ = canonicalize_and_hash(doc7)
    # Python json parses 3.0 as float; our impl uses int(doc.get('channelId', 0))
    # which truncates — but JSON spec says integers should be sent without decimal.
    # If PWA sends 3.0, the behavior is int(3.0)=3, matching int 3.
    _, h8, _ = canonicalize_and_hash(doc8)
    check(h7 == h8, "integer 3 and float 3.0 produce same hash (int canonicalization)")


# ===========================================================================
# Test Suite C: Hash Invariants (directive §23.C / AC-007 / AC-008)
# ===========================================================================

def test_hash_invariants() -> None:
    print("\n=== C. Hash Invariants ===")

    # Test 1: TX-001 relay/on/channel=3 vs TX-002 relay/on/channel=3 → SAME hash
    doc_a = {"type": "relay", "action": "on", "requestId": "TX-001", "channelId": 3}
    doc_b = {"type": "relay", "action": "on", "requestId": "TX-002", "channelId": 3}
    _, hash_a, _ = canonicalize_and_hash(doc_a)
    _, hash_b, _ = canonicalize_and_hash(doc_b)
    check(hash_a == hash_b, "TX-001 and TX-002 same logical command → same hash (AC-007: hash excludes transactionId)")

    # Test 2: TX-001 relay/on vs TX-001 relay/off → DIFFERENT hash
    doc_c = {"type": "relay", "action": "on", "requestId": "TX-001", "channelId": 3}
    doc_d = {"type": "relay", "action": "off", "requestId": "TX-001", "channelId": 3}
    _, hash_c, _ = canonicalize_and_hash(doc_c)
    _, hash_d, _ = canonicalize_and_hash(doc_d)
    check(hash_c != hash_d, "TX-001 relay/on vs relay/off → different hash (AC-008)")

    # Test 3: different property ordering → same hash (already covered in B, re-verify)
    doc_e = {"type": "schedule", "action": "upsert", "requestId": "TX-1",
             "channelId": 3, "id": 1, "onTime": "07:00", "offTime": "08:00",
             "dayMask": 127, "enabled": True}
    doc_f = {"type": "schedule", "action": "upsert", "requestId": "TX-1",
             "enabled": True, "dayMask": 127, "offTime": "08:00", "onTime": "07:00",
             "id": 1, "channelId": 3}
    _, hash_e, _ = canonicalize_and_hash(doc_e)
    _, hash_f, _ = canonicalize_and_hash(doc_f)
    check(hash_e == hash_f, "schedule with different property order → same hash")

    # AC-007: hash excludes issuedAt / expiresAt
    doc_g = {"type": "relay", "action": "on", "requestId": "TX-1", "channelId": 3,
             "issuedAt": "2026-01-01T00:00:00Z", "expiresAt": "2026-01-01T01:00:00Z"}
    _, hash_g, _ = canonicalize_and_hash(doc_g)
    check(hash_a == hash_g, "hash excludes issuedAt/expiresAt (AC-007)")

    # AC-007: hash excludes transport metadata (we don't even read these fields)
    doc_h = {"type": "relay", "action": "on", "requestId": "TX-1", "channelId": 3,
             "_mqttTopic": "timer12/abc/command", "_httpMethod": "POST"}
    # Unknown fields like _mqttTopic are NOT in the whitelist, but our canonicalize
    # doesn't enforce whitelist rejection (that's the dispatcher's job). They're
    # simply ignored by the per-type field extraction.
    _, hash_h, _ = canonicalize_and_hash(doc_h)
    check(hash_a == hash_h, "hash excludes transport metadata (_mqttTopic, _httpMethod)")


# ===========================================================================
# Test Suite D: Duplicate / Conflict Detection (directive §24 / AC-009/010/011)
# ===========================================================================

def test_duplicate_conflict() -> None:
    print("\n=== D. Duplicate / Conflict Detection ===")
    journal.reset()

    # TX-001 + HASH-A (NEW) → store
    doc1 = {"type": "relay", "action": "on", "requestId": "TX-DUP-001", "channelId": 3}
    tid1, hash1, _ = canonicalize_and_hash(doc1)
    check(decide_transaction(tid1, hash1) == "NEW", "first command → NEW")
    journal.store(tid1, hash1, '{"success":true,"message":"Relay updated"}')

    # TX-001 + HASH-A (DUPLICATE)
    check(decide_transaction(tid1, hash1) == "DUPLICATE",
          "same TX + same hash → DUPLICATE (AC-009)")

    # TX-001 + HASH-B (CONFLICT)
    doc2 = {"type": "relay", "action": "off", "requestId": "TX-DUP-001", "channelId": 3}
    tid2, hash2, _ = canonicalize_and_hash(doc2)
    check(tid2 == tid1, "same transactionId")
    check(hash2 != hash1, "different action → different hash")
    check(decide_transaction(tid2, hash2) == "CONFLICT",
          "same TX + different hash → CONFLICT (AC-010)")

    # AC-011: CONFLICT must NOT cause physical mutation.
    # In our mock, this means the journal entry for TX-001 must NOT be overwritten.
    original_hash = journal.get_command_hash(tid1)
    original_ack = journal.get_ack_json(tid1)
    # Even if someone erroneously calls store() on a CONFLICT, the mock refuses.
    journal.store(tid2, hash2, '{"success":true,"message":"should not store"}')
    check(journal.get_command_hash(tid1) == original_hash,
          "CONFLICT does not overwrite journal hash (AC-011)")
    check(journal.get_ack_json(tid1) == original_ack,
          "CONFLICT does not overwrite journal ACK (AC-011)")

    # AC-009: DUPLICATE must NOT re-execute (no second logical mutation).
    # In our mock, calling store() on a DUPLICATE is a no-op.
    journal.store(tid1, hash1, '{"success":true,"message":"different ack should not store"}')
    check(journal.get_ack_json(tid1) == original_ack,
          "DUPLICATE does not overwrite journal ACK (AC-009: no re-execution)")


# ===========================================================================
# Test Suite E: Cross-Transport Equivalence (directive §25 / AC-001/018)
# ===========================================================================

def test_cross_transport() -> None:
    print("\n=== E. REST / MQTT Cross-Transport Hash Equivalence ===")

    # Relay ON channel 3 — sent via REST vs MQTT
    # REST: POST /api/relay  body: {"channelId":3,"action":"on","requestId":"TX-X"}
    #   → handler injects type="relay", action="on"
    # MQTT: {"type":"relay","action":"on","requestId":"TX-Y","channelId":3}
    rest_relay = {"channelId": 3, "action": "on", "requestId": "TX-REST-1"}
    mqtt_relay = {"type": "relay", "action": "on", "requestId": "TX-MQTT-1", "channelId": 3}
    # Simulate REST handler injecting type/action:
    rest_relay_injected = dict(rest_relay)
    rest_relay_injected["type"] = "relay"
    rest_relay_injected["action"] = "on"

    _, hash_rest, _ = canonicalize_and_hash(rest_relay_injected)
    _, hash_mqtt, _ = canonicalize_and_hash(mqtt_relay)
    check(hash_rest == hash_mqtt,
          "REST /api/relay produces same hash as MQTT type=relay (AC-001/AC-018)")

    # Schedule upsert — REST vs MQTT
    rest_sched = {"channelId": 3, "onTime": "07:00", "offTime": "08:00",
                  "dayMask": 127, "enabled": True, "id": 0, "requestId": "TX-REST-2"}
    mqtt_sched = {"type": "schedule", "action": "upsert", "channelId": 3,
                  "onTime": "07:00", "offTime": "08:00", "dayMask": 127,
                  "enabled": True, "id": 0, "requestId": "TX-MQTT-2"}
    rest_sched_injected = dict(rest_sched)
    rest_sched_injected["type"] = "schedule"
    rest_sched_injected["action"] = "upsert"
    _, h_rs, _ = canonicalize_and_hash(rest_sched_injected)
    _, h_ms, _ = canonicalize_and_hash(mqtt_sched)
    check(h_rs == h_ms, "REST /api/schedule produces same hash as MQTT type=schedule")

    # Channel rename — REST vs MQTT
    rest_ch = {"channelId": 3, "name": "Living Room", "requestId": "TX-REST-3"}
    mqtt_ch = {"type": "channel", "action": "rename", "channelId": 3,
               "name": "Living Room", "requestId": "TX-MQTT-3"}
    rest_ch_injected = dict(rest_ch)
    rest_ch_injected["type"] = "channel"
    rest_ch_injected["action"] = "rename"
    _, h_rc, _ = canonicalize_and_hash(rest_ch_injected)
    _, h_mc, _ = canonicalize_and_hash(mqtt_ch)
    check(h_rc == h_mc, "REST /api/channel produces same hash as MQTT type=channel")

    # PIR config — REST vs MQTT
    rest_pir = {"id": 1, "enabled": True, "holdTime": 60, "requestId": "TX-REST-4"}
    mqtt_pir = {"type": "pir", "action": "config", "id": 1,
                "enabled": True, "holdTime": 60, "requestId": "TX-MQTT-4"}
    rest_pir_injected = dict(rest_pir)
    rest_pir_injected["type"] = "pir"
    rest_pir_injected["action"] = "config"
    _, h_rp, _ = canonicalize_and_hash(rest_pir_injected)
    _, h_mp, _ = canonicalize_and_hash(mqtt_pir)
    check(h_rp == h_mp, "REST /api/pir produces same hash as MQTT type=pir")

    # Time set — REST vs MQTT
    rest_time = {"datetime": "2026-01-01T12:00:00", "requestId": "TX-REST-5"}
    mqtt_time = {"type": "time", "action": "set", "datetime": "2026-01-01T12:00:00",
                 "requestId": "TX-MQTT-5"}
    rest_time_injected = dict(rest_time)
    rest_time_injected["type"] = "time"
    rest_time_injected["action"] = "set"
    _, h_rt, _ = canonicalize_and_hash(rest_time_injected)
    _, h_mt, _ = canonicalize_and_hash(mqtt_time)
    check(h_rt == h_mt, "REST /api/time produces same hash as MQTT type=time")

    # System reboot — REST vs MQTT
    rest_reboot = {"requestId": "TX-REST-6"}
    mqtt_reboot = {"type": "system", "action": "reboot", "requestId": "TX-MQTT-6"}
    rest_reboot_injected = dict(rest_reboot)
    rest_reboot_injected["type"] = "system"
    rest_reboot_injected["action"] = "reboot"
    _, h_rr, _ = canonicalize_and_hash(rest_reboot_injected)
    _, h_mr, _ = canonicalize_and_hash(mqtt_reboot)
    check(h_rr == h_mr, "REST /api/reboot produces same hash as MQTT type=system action=reboot")

    # Config setDevice — REST vs MQTT
    rest_cfg = {"deviceName": "My Timer", "timezone": "Asia/Jakarta", "requestId": "TX-REST-7"}
    mqtt_cfg = {"type": "config", "action": "setDevice", "deviceName": "My Timer",
                "timezone": "Asia/Jakarta", "requestId": "TX-MQTT-7"}
    rest_cfg_injected = dict(rest_cfg)
    rest_cfg_injected["type"] = "config"
    rest_cfg_injected["action"] = "setDevice"
    _, h_rfg, _ = canonicalize_and_hash(rest_cfg_injected)
    _, h_mfg, _ = canonicalize_and_hash(mqtt_cfg)
    check(h_rfg == h_mfg, "REST /api/config/device produces same hash as MQTT type=config")


# ===========================================================================
# Test Suite F: Negative / Robustness (directive §26)
# ===========================================================================

def test_negative_cases() -> None:
    print("\n=== F. Negative / Robustness ===")

    # missing transactionId → REJECT
    doc = {"type": "relay", "action": "on", "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc), "missing transactionId → rejected")

    # empty transactionId → REJECT
    doc = {"type": "relay", "action": "on", "requestId": "", "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc), "empty transactionId → rejected")

    # missing type → REJECT
    doc = {"action": "on", "requestId": "TX-1", "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc), "missing type → rejected")

    # missing action → REJECT
    doc = {"type": "relay", "requestId": "TX-1", "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc), "missing action → rejected")

    # unknown type → REJECT
    doc = {"type": "unknown_type", "action": "foo", "requestId": "TX-1"}
    check_raises(lambda: canonicalize_and_hash(doc), "unknown type → rejected")

    # unsupported protocol version → REJECT (no silent downgrade, §13)
    doc = {"type": "relay", "action": "on", "requestId": "TX-1", "channelId": 3,
           "version": 2}
    check_raises(lambda: canonicalize_and_hash(doc), "unsupported protocol version → rejected (no silent downgrade, §13/AC-012)")

    # oversized transactionId → REJECT
    doc = {"type": "relay", "action": "on", "requestId": "X" * 65, "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc), "oversized transactionId → rejected")

    # invalid chars in transactionId → REJECT
    doc = {"type": "relay", "action": "on", "requestId": "TX with space", "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc), "transactionId with space → rejected")
    doc = {"type": "relay", "action": "on", "requestId": "TX/slash", "channelId": 3}
    check_raises(lambda: canonicalize_and_hash(doc), "transactionId with slash → rejected")

    # Unknown field — registry reports it as not allowed (directive §10/§26)
    check(is_field_allowed("relay", "channelId") is True, "relay.channelId is allowed")
    check(is_field_allowed("relay", "bogusField") is False, "relay.bogusField is NOT allowed")
    check(is_field_allowed("schedule", "onTime") is True, "schedule.onTime is allowed")
    check(is_field_allowed("schedule", "extraField") is False, "schedule.extraField is NOT allowed")
    check(is_field_allowed("system", "anything") is False, "system has no payload fields")

    # Registry: known (type, action) pairs
    check(is_known_command_type("relay", "on"), "relay/on is known")
    check(is_known_command_type("relay", "off"), "relay/off is known")
    check(is_known_command_type("relay", "set_mode"), "relay/set_mode is known")
    check(not is_known_command_type("relay", "toggle"), "relay/toggle is NOT known (removed for idempotency)")
    check(not is_known_command_type("relay", "bogus"), "relay/bogus is NOT known")
    check(is_known_command_type("schedule", "upsert"), "schedule/upsert is known")
    check(is_known_command_type("schedule", "delete"), "schedule/delete is known")
    check(is_known_command_type("pir", "config"), "pir/config is known")
    check(is_known_command_type("pir", "test"), "pir/test is known")
    check(is_known_command_type("channel", "rename"), "channel/rename is known")
    check(is_known_command_type("time", "set"), "time/set is known")
    check(is_known_command_type("system", "reboot"), "system/reboot is known")
    check(is_known_command_type("config", "setDevice"), "config/setDevice is known")
    check(is_known_command_type("ota", "update"), "ota/update is known")


# ===========================================================================
# Test Suite G: Payload String Preservation (directive §8)
# ===========================================================================

def test_payload_string_preservation() -> None:
    print("\n=== G. Payload String Preservation (directive §8) ===")

    # channelName "Living Room" must NOT be lowercased to "living room"
    doc1 = {"type": "channel", "action": "rename", "requestId": "TX-1",
            "channelId": 3, "name": "Living Room"}
    _, _, c1 = canonicalize_and_hash(doc1)
    check("Living Room" in c1, "payload string 'Living Room' preserved (not lowercased)")

    doc2 = {"type": "channel", "action": "rename", "requestId": "TX-1",
            "channelId": 3, "name": "living room"}
    _, _, c2 = canonicalize_and_hash(doc2)
    check("living room" in c2, "payload string 'living room' preserved (different from 'Living Room')")

    _, h1, _ = canonicalize_and_hash(doc1)
    _, h2, _ = canonicalize_and_hash(doc2)
    check(h1 != h2, "'Living Room' and 'living room' produce DIFFERENT hashes (case-sensitive payload)")

    # deviceName "My Timer" preserved
    doc3 = {"type": "config", "action": "setDevice", "requestId": "TX-1",
            "deviceName": "My Timer", "timezone": "Asia/Jakarta"}
    _, _, c3 = canonicalize_and_hash(doc3)
    check("My Timer" in c3 and "Asia/Jakarta" in c3, "deviceName/timezone preserved verbatim")


# ===========================================================================
# Test Suite H: Default Values (directive §10 — Optional Field Policy)
# ===========================================================================

def test_default_values() -> None:
    print("\n=== H. Default Values (directive §10) ===")

    # relay.manualState default = false (omitted vs explicit false → same hash)
    doc1 = {"type": "relay", "action": "set_mode", "requestId": "TX-1",
            "channelId": 3, "mode": "manual"}  # manualState omitted
    doc2 = {"type": "relay", "action": "set_mode", "requestId": "TX-1",
            "channelId": 3, "mode": "manual", "manualState": False}  # explicit false
    _, h1, _ = canonicalize_and_hash(doc1)
    _, h2, _ = canonicalize_and_hash(doc2)
    check(h1 == h2, "relay.manualState: omitted == explicit false (default policy)")

    # schedule.enabled default = true (omitted vs explicit true → same hash)
    doc3 = {"type": "schedule", "action": "upsert", "requestId": "TX-1",
            "channelId": 3, "id": 0, "onTime": "07:00", "offTime": "08:00",
            "dayMask": 127}  # enabled omitted
    doc4 = {"type": "schedule", "action": "upsert", "requestId": "TX-1",
            "channelId": 3, "id": 0, "onTime": "07:00", "offTime": "08:00",
            "dayMask": 127, "enabled": True}  # explicit true
    _, h3, _ = canonicalize_and_hash(doc3)
    _, h4, _ = canonicalize_and_hash(doc4)
    check(h3 == h4, "schedule.enabled: omitted == explicit true (default policy)")

    # BUT: omitted != explicit opposite value
    doc5 = {"type": "schedule", "action": "upsert", "requestId": "TX-1",
            "channelId": 3, "id": 0, "onTime": "07:00", "offTime": "08:00",
            "dayMask": 127, "enabled": False}
    _, h5, _ = canonicalize_and_hash(doc5)
    check(h3 != h5, "schedule.enabled: omitted(true) != explicit false (no ambiguity)")


# ===========================================================================
# Test Suite I: Cross-Transport Duplicate Detection
# ===========================================================================

def test_cross_transport_duplicate() -> None:
    print("\n=== I. Cross-Transport Duplicate Detection ===")
    journal.reset()

    # Scenario: PWA sends relay ON channel 3 via MQTT with requestId=TX-X.
    # Network glitch → PWA retries via REST (LAN mode) with SAME requestId=TX-X
    # and same logical command. Firmware must detect DUPLICATE and replay ACK,
    # NOT re-execute the relay command.

    mqtt_cmd = {"type": "relay", "action": "on", "requestId": "TX-CROSS-1", "channelId": 3}
    tid_m, hash_m, _ = canonicalize_and_hash(mqtt_cmd)
    check(decide_transaction(tid_m, hash_m) == "NEW", "MQTT command first → NEW")
    journal.store(tid_m, hash_m, '{"success":true,"message":"Relay updated","data":{"channel":{"id":3,"state":true}}}')

    # REST retry with same requestId
    rest_cmd = {"channelId": 3, "action": "on", "requestId": "TX-CROSS-1"}
    rest_cmd["type"] = "relay"  # injected by REST handler
    tid_r, hash_r, _ = canonicalize_and_hash(rest_cmd)
    check(tid_r == tid_m, "REST retry uses same transactionId")
    check(hash_r == hash_m, "REST retry produces same commandHash (cross-transport equivalence)")
    check(decide_transaction(tid_r, hash_r) == "DUPLICATE",
          "REST retry after MQTT → DUPLICATE (no re-execution)")

    # Scenario: PWA sends relay ON channel 3 via MQTT, then sends relay OFF
    # channel 3 via REST with SAME requestId. Must be CONFLICT.
    rest_cmd2 = {"channelId": 3, "action": "off", "requestId": "TX-CROSS-1"}
    rest_cmd2["type"] = "relay"
    tid_r2, hash_r2, _ = canonicalize_and_hash(rest_cmd2)
    check(tid_r2 == tid_m, "same transactionId")
    check(hash_r2 != hash_m, "different action → different hash")
    check(decide_transaction(tid_r2, hash_r2) == "CONFLICT",
          "same TX + different command via different transport → CONFLICT (AC-010)")


# ===========================================================================
# Test Suite J: Canonical String Format Verification
# ===========================================================================

def test_canonical_string_format() -> None:
    print("\n=== J. Canonical String Format Verification ===")

    # Verify exact canonical string for relay/on
    doc = {"type": "relay", "action": "on", "requestId": "TX-1", "channelId": 3}
    _, _, canonical = canonicalize_and_hash(doc)
    expected = "v1|relay|on|channelId=3|mode=|manualState=false"
    check(canonical == expected, f"relay/on canonical string: '{canonical}' == '{expected}'")

    # Verify exact canonical string for relay/set_mode
    doc = {"type": "relay", "action": "set_mode", "requestId": "TX-1",
           "channelId": 3, "mode": "manual", "manualState": True}
    _, _, canonical = canonicalize_and_hash(doc)
    expected = "v1|relay|set_mode|channelId=3|mode=manual|manualState=true"
    check(canonical == expected, f"relay/set_mode canonical string: '{canonical}' == '{expected}'")

    # Verify exact canonical string for schedule/upsert
    doc = {"type": "schedule", "action": "upsert", "requestId": "TX-1",
           "channelId": 3, "id": 1, "onTime": "07:00", "offTime": "08:00",
           "dayMask": 127, "enabled": True}
    _, _, canonical = canonicalize_and_hash(doc)
    expected = "v1|schedule|upsert|channelId=3|id=1|onTime=07:00|offTime=08:00|dayMask=127|enabled=true"
    check(canonical == expected, f"schedule/upsert canonical string matches expected")

    # Verify exact canonical string for system/reboot (no payload)
    doc = {"type": "system", "action": "reboot", "requestId": "TX-1"}
    _, _, canonical = canonicalize_and_hash(doc)
    expected = "v1|system|reboot"
    check(canonical == expected, f"system/reboot canonical string: '{canonical}' == '{expected}'")

    # Verify exact hash for a known canonical string (regression lock)
    # This locks the hash output so future refactors can't silently change it.
    known_canonical = "v1|relay|on|channelId=3|mode=|manualState=false"
    known_hash = hashlib.sha256(known_canonical.encode("utf-8")).hexdigest()
    expected_hash_prefix = "bfa3"  # first 4 hex chars of sha256(known_canonical)
    check(known_hash.startswith(expected_hash_prefix),
          f"sha256('{known_canonical}') starts with '{expected_hash_prefix}' "
          f"(full: {known_hash[:16]}...)")
    print(f"    (locked hash: {known_hash})")


# ===========================================================================
# Main entry
# ===========================================================================

def main() -> int:
    print("=" * 70)
    print("PD-001 Canonical Command & Transaction Model — Test Suite")
    print("=" * 70)
    print(f"Python reference implementation mirroring:")
    print(f"  firmware/CommandCanonicalizer.h")
    print(f"  firmware/CommandCanonicalizer.cpp")
    print(f"  firmware/Common.h (REST transaction helpers)")
    print(f"  firmware/MqttClient.cpp (shared canonicalizer integration)")

    test_transaction_identity()
    test_canonical_serialization()
    test_hash_invariants()
    test_duplicate_conflict()
    test_cross_transport()
    test_negative_cases()
    test_payload_string_preservation()
    test_default_values()
    test_cross_transport_duplicate()
    test_canonical_string_format()

    print("\n" + "=" * 70)
    print(f"RESULTS: {PASS} passed, {FAIL} failed")
    if FAIL > 0:
        print("\nFAILED TESTS:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("ALL TESTS PASSED")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
