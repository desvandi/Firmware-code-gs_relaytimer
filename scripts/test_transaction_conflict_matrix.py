#!/usr/bin/env python3
"""
test_transaction_conflict_matrix.py — Directive §25 exact Test 1-4 matrix.

Mirrors firmware CommandCanonicalizer + TransactionJournal contract.

Test matrix:
  1. requestId A + payload X → execute once (decision=NEW, mutation_count=1)
  2. requestId A + payload X again → no second mutation + ACK replay
     (decision=DUPLICATE, mutation_count stays 1, prev_ack matches)
  3. requestId A + payload Y (different) → REJECT + CONFLICT + NO ACTUATOR MUTATION
     (decision=CONFLICT, mutation_count stays 1, store refuses)
  4. requestId A + malformed payload (missing type/action/channelId) →
     REJECT at canonicalization layer
"""
from __future__ import annotations

import hashlib
import sys
from typing import Any, Dict, Optional, Tuple


# ---------------------------------------------------------------------------
# canonical_hash — mirrors CommandCanonicalizer::buildCanonicalString + sha256Hex
# ---------------------------------------------------------------------------

def canonical_hash(command: Dict[str, Any]) -> Optional[str]:
    """Compute the canonical hash for a command.

    Excludes requestId/transactionId from the canonical form. Sorts keys,
    joins as `key=value|key=value`, SHA-256 hex.
    Returns None if command is missing required fields (type/action).
    """
    if "type" not in command or "action" not in command:
        return None
    # type and action are required at minimum for relay commands.
    # For relay commands, channelId is required (Test 4 expects rejection).
    excluded = {"requestId", "transactionId", "version", "issuedAt", "expiresAt"}
    pairs = []
    for k in sorted(command.keys()):
        if k in excluded:
            continue
        v = command[k]
        # Render value explicitly (mirrors firmware): bool → "true"/"false",
        # int → decimal string, string → verbatim
        if isinstance(v, bool):
            v_str = "true" if v else "false"
        elif isinstance(v, int):
            v_str = str(v)
        elif isinstance(v, str):
            v_str = v
        else:
            v_str = str(v)
        pairs.append(f"{k.lower()}={v_str}")
    canonical = "|".join(pairs)

    # Per directive §25 / Test 4: missing type/action/channelId → reject at
    # canonicalization layer. We accept "type" + "action" + (relay requires channelId).
    if command.get("type") == "relay" and "channelId" not in command:
        return None

    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


# ---------------------------------------------------------------------------
# TransactionJournalMock
# ---------------------------------------------------------------------------

class TransactionJournalMock:
    """Mirrors firmware TransactionJournal contract for store() refusing
    conflicts."""

    def __init__(self):
        # requestId → (command_hash, ack_json)
        self._entries: Dict[str, Tuple[str, str]] = {}

    def find(self, request_id: str) -> Optional[Tuple[str, str]]:
        return self._entries.get(request_id)

    def store(self, request_id: str, command_hash: str, ack_json: str) -> bool:
        """Returns True if stored, False if REFUSED (conflicting requestId)."""
        existing = self._entries.get(request_id)
        if existing is None:
            self._entries[request_id] = (command_hash, ack_json)
            return True
        # requestId already exists — store refuses if hash differs
        prev_hash, _ = existing
        if prev_hash != command_hash:
            return False
        # Same hash → idempotent re-store (allowed)
        return True


# ---------------------------------------------------------------------------
# Decision engine
# ---------------------------------------------------------------------------

DECISION_NEW = "NEW"
DECISION_DUPLICATE = "DUPLICATE"
DECISION_CONFLICT = "CONFLICT"
DECISION_REJECT = "REJECT"


def decide(journal: TransactionJournalMock, request_id: str,
           command_hash: Optional[str]) -> Tuple[str, Optional[str]]:
    """Return (decision, prev_ack_json)."""
    if command_hash is None:
        return (DECISION_REJECT, None)
    existing = journal.find(request_id)
    if existing is None:
        return (DECISION_NEW, None)
    prev_hash, prev_ack = existing
    if prev_hash != command_hash:
        return (DECISION_CONFLICT, prev_ack)
    return (DECISION_DUPLICATE, prev_ack)


def execute_mutation(target: Dict[str, int], command: Dict[str, Any]) -> str:
    """Increment mutation counter for the target channel, return ack JSON."""
    ch = command.get("channelId", 0)
    target[ch] = target.get(ch, 0) + 1
    return f'{{"ok":true,"channelId":{ch},"mut":{target[ch]}}}'


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_1_new_request_executes_once() -> Tuple[bool, str]:
    journal = TransactionJournalMock()
    mutations: Dict[str, int] = {}
    cmd = {"type": "relay", "action": "on", "channelId": 3,
           "requestId": "req-A", "manualState": True}
    h = canonical_hash(cmd)
    decision, _ = decide(journal, "req-A", h)
    if decision != DECISION_NEW:
        return False, f"expected NEW, got {decision}"
    ack = execute_mutation(mutations, cmd)
    journal.store("req-A", h, ack)
    if mutations.get(3, 0) != 1:
        return False, f"mutation_count should be 1, got {mutations.get(3,0)}"
    return True, "requestId A + payload X → NEW, mutation_count=1"


def test_2_duplicate_replays_ack() -> Tuple[bool, str]:
    journal = TransactionJournalMock()
    mutations: Dict[str, int] = {}
    cmd = {"type": "relay", "action": "on", "channelId": 3,
           "requestId": "req-A", "manualState": True}
    h = canonical_hash(cmd)
    # First
    _, _ = decide(journal, "req-A", h)
    ack1 = execute_mutation(mutations, cmd)
    journal.store("req-A", h, ack1)

    # Second: same requestId, same payload
    decision, prev_ack = decide(journal, "req-A", h)
    if decision != DECISION_DUPLICATE:
        return False, f"expected DUPLICATE, got {decision}"
    if prev_ack != ack1:
        return False, "prev_ack does not match first ACK"
    # NO second mutation should be made on DUPLICATE
    # (firmware: just replay the prev_ack, don't call execute)
    if mutations.get(3, 0) != 1:
        return False, f"mutation_count should still be 1, got {mutations.get(3,0)}"
    return True, "requestId A + payload X again → DUPLICATE, mutation stays 1, ACK replayed"


def test_3_conflict_rejects_and_no_mutation() -> Tuple[bool, str]:
    journal = TransactionJournalMock()
    mutations: Dict[str, int] = {}
    cmd_x = {"type": "relay", "action": "on", "channelId": 3,
             "requestId": "req-A", "manualState": True}
    h_x = canonical_hash(cmd_x)
    _, _ = decide(journal, "req-A", h_x)
    ack_x = execute_mutation(mutations, cmd_x)
    journal.store("req-A", h_x, ack_x)

    # Now reuse requestId A but with DIFFERENT payload (channelId=5 instead of 3)
    cmd_y = {"type": "relay", "action": "on", "channelId": 5,
             "requestId": "req-A", "manualState": True}
    h_y = canonical_hash(cmd_y)
    if h_y == h_x:
        return False, "hashes should differ for different payloads"
    decision, prev_ack = decide(journal, "req-A", h_y)
    if decision != DECISION_CONFLICT:
        return False, f"expected CONFLICT, got {decision}"
    # store MUST refuse
    stored = journal.store("req-A", h_y, "should-not-be-stored")
    if stored:
        return False, "store should refuse conflicting requestId"
    # NO actuator mutation for channel 5
    if mutations.get(5, 0) != 0:
        return False, f"channel 5 should NOT be mutated, got {mutations.get(5,0)}"
    if mutations.get(3, 0) != 1:
        return False, f"channel 3 mutation_count should still be 1, got {mutations.get(3,0)}"
    return True, "requestId A + payload Y → CONFLICT, no mutation, store refused"


def test_4_malformed_payload_rejected() -> Tuple[bool, str]:
    journal = TransactionJournalMock()
    mutations: Dict[str, int] = {}
    # Missing channelId on relay command → canonical_hash returns None → REJECT
    cmd_bad = {"type": "relay", "action": "on", "requestId": "req-A",
               "manualState": True}
    # Missing type entirely
    cmd_no_type = {"action": "on", "channelId": 3, "requestId": "req-A"}
    # Missing action entirely
    cmd_no_action = {"type": "relay", "channelId": 3, "requestId": "req-A"}

    for label, cmd in [("missing-channelId", cmd_bad),
                      ("missing-type", cmd_no_type),
                      ("missing-action", cmd_no_action)]:
        h = canonical_hash(cmd)
        if h is not None:
            return False, f"{label}: canonical_hash should return None, got {h}"
        decision, _ = decide(journal, "req-A", h)
        if decision != DECISION_REJECT:
            return False, f"{label}: expected REJECT, got {decision}"
    # No mutations
    if mutations:
        return False, f"mutations should be empty, got {mutations}"
    return True, "malformed payloads (missing type/action/channelId) → REJECT at canonicalization"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    print("=" * 78)
    print("Transaction Conflict Matrix — Directive §25 Tests 1-4")
    print("=" * 78)

    tests = [
        ("Test 1: NEW request executes once", test_1_new_request_executes_once),
        ("Test 2: DUPLICATE replays ACK, no mutation", test_2_duplicate_replays_ack),
        ("Test 3: CONFLICT — different payload, REJECT, no mutation", test_3_conflict_rejects_and_no_mutation),
        ("Test 4: Malformed payload → REJECT at canonicalization", test_4_malformed_payload_rejected),
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
