#!/usr/bin/env python3
"""
test_mqtt_acl_isolation.py — Mosquitto ACL device isolation test.

Embeds an ACL_CONFIG string and tests 9 device isolation cases:
  1. device-A → A/status (read) = ALLOW
  2. device-A → B/status (read) = DENY
  3. device-B → B/status (read) = ALLOW
  4. device-B → A/status (read) = DENY
  5. device-A → unknown device C7/status (read) = DENY (fleet wildcard blocked)
  6. device-A → B/command (write) = DENY (cross-device command blocked)
  7. pwa-user-001 → unauthorized C7/status (read) = DENY
  8. pwa-user-001 → A/status (read) = ALLOW, A/command (write) = ALLOW,
     A/status (write) = DENY (PWA cannot impersonate device)
  9. anonymous (empty username) → A/status (read) = DENY
"""
from __future__ import annotations

import sys
from typing import Dict, List, Tuple


# ---------------------------------------------------------------------------
# ACL config — mirrors mosquitto_acl.conf
# ---------------------------------------------------------------------------

ACL_CONFIG = """# Mosquitto ACL Configuration — Per-Device Topic Isolation
# Timer Digital Relay v4.3.8

# Device A
user device-A4B1C2D3E4F5
  topic readwrite timer12/A4B1C2D3E4F5/#

# Device B
user device-B5C2D3E4F5A6
  topic readwrite timer12/B5C2D3E4F5A6/#

# PWA user with access to both A and B
user pwa-user-001
  topic read timer12/A4B1C2D3E4F5/status
  topic read timer12/A4B1C2D3E4F5/log
  topic read timer12/A4B1C2D3E4F5/online
  topic read timer12/A4B1C2D3E4F5/ack
  topic write timer12/A4B1C2D3E4F5/command
  topic write timer12/A4B1C2D3E4F5/ota
  topic read timer12/B5C2D3E4F5A6/status
  topic read timer12/B5C2D3E4F5A6/log
  topic read timer12/B5C2D3E4F5A6/online
  topic read timer12/B5C2D3E4F5A6/ack
  topic write timer12/B5C2D3E4F5A6/command
  topic write timer12/B5C2D3E4F5A6/ota

# Pattern directive: %u is the username.
# pattern readwrite timer12/%u/#
"""


# ---------------------------------------------------------------------------
# MosquittoACL parser
# ---------------------------------------------------------------------------

class ACLEntry:
    __slots__ = ("topic", "action")

    def __init__(self, topic: str, action: str):
        self.topic = topic
        self.action = action  # "read", "write", "readwrite"


class ACLPattern:
    __slots__ = ("topic", "action")

    def __init__(self, topic: str, action: str):
        self.topic = topic  # may contain %u
        self.action = action


class MosquittoACL:
    def __init__(self):
        # username → list of ACLEntry
        self.user_entries: Dict[str, List[ACLEntry]] = {}
        self.patterns: List[ACLPattern] = []

    def parse(self, text: str) -> None:
        current_user: str = ""
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            tokens = line.split()
            if not tokens:
                continue
            if tokens[0] == "user":
                if len(tokens) < 2:
                    current_user = ""
                else:
                    current_user = tokens[1]
                if current_user not in self.user_entries:
                    self.user_entries[current_user] = []
            elif tokens[0] == "topic":
                if len(tokens) < 3:
                    continue
                action = tokens[1].lower()
                topic = tokens[2]
                if current_user == "" and "%" not in topic:
                    # Global topic — apply to all users (rare in this config)
                    # For our purposes, skip if no user set
                    continue
                self.user_entries.setdefault(current_user, []).append(
                    ACLEntry(topic, action)
                )
            elif tokens[0] == "pattern":
                if len(tokens) >= 3:
                    self.patterns.append(ACLPattern(tokens[2], tokens[1].lower()))

    # ---- Topic matching (Mosquitto wildcard semantics) ----

    @staticmethod
    def _topic_matches(pattern: str, topic: str) -> bool:
        """Check if `topic` matches Mosquitto wildcard `pattern`.

        `#` matches rest (including / levels), `+` matches one level.
        """
        p_parts = pattern.split("/")
        t_parts = topic.split("/")
        i = 0
        while i < len(p_parts):
            p = p_parts[i]
            if p == "#":
                return True  # matches rest (any depth including 0)
            if i >= len(t_parts):
                return False
            t = t_parts[i]
            if p == "+":
                # matches exactly one level
                pass
            elif p != t:
                return False
            i += 1
        return i == len(t_parts)

    def check(self, username: str, topic: str, action: str) -> bool:
        """Default deny. Returns True if (username, topic, action) is allowed."""
        action = action.lower()
        # User-specific entries
        entries = self.user_entries.get(username, [])
        for e in entries:
            if self._topic_matches(e.topic, topic):
                if (e.action == "readwrite"
                    or e.action == action):
                    return True
        # Pattern entries (substitute %u)
        for pat in self.patterns:
            expanded = pat.topic.replace("%u", username)
            if self._topic_matches(expanded, topic):
                if (pat.action == "readwrite" or pat.action == action):
                    return True
        # Default deny
        return False


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

DEVICE_A = "device-A4B1C2D3E4F5"
DEVICE_B = "device-B5C2D3E4F5A6"
PWA_USER = "pwa-user-001"
UNKNOWN_DEVICE = "device-C7C7C7C7C7C7"

TESTS = [
    # (name, username, topic, action, expected)
    ("1. device-A → A/status (read) = ALLOW",
     DEVICE_A, "timer12/A4B1C2D3E4F5/status", "read", True),
    ("2. device-A → B/status (read) = DENY",
     DEVICE_A, "timer12/B5C2D3E4F5A6/status", "read", False),
    ("3. device-B → B/status (read) = ALLOW",
     DEVICE_B, "timer12/B5C2D3E4F5A6/status", "read", True),
    ("4. device-B → A/status (read) = DENY",
     DEVICE_B, "timer12/A4B1C2D3E4F5/status", "read", False),
    ("5. device-A → unknown device C7/status (read) = DENY",
     DEVICE_A, "timer12/C7C7C7C7C7C7/status", "read", False),
    ("6. device-A → B/command (write) = DENY",
     DEVICE_A, "timer12/B5C2D3E4F5A6/command", "write", False),
    ("7. pwa-user-001 → unauthorized C7/status (read) = DENY",
     PWA_USER, "timer12/C7C7C7C7C7C7/status", "read", False),
    # Test 8 is checked separately (3-way)
    ("9. anonymous → A/status (read) = DENY",
     "", "timer12/A4B1C2D3E4F5/status", "read", False),
]


def test_8_pwa_user_matrix(acl: MosquittoACL) -> Tuple[bool, str]:
    """PWA user: ALLOW A/status read + A/command write, DENY A/status write."""
    cases = [
        ("timer12/A4B1C2D3E4F5/status", "read", True),
        ("timer12/A4B1C2D3E4F5/command", "write", True),
        ("timer12/A4B1C2D3E4F5/status", "write", False),  # cannot impersonate device
    ]
    for topic, action, expected in cases:
        actual = acl.check(PWA_USER, topic, action)
        if actual != expected:
            return False, f"PWA user {topic} ({action}): expected={expected}, actual={actual}"
    return True, "pwa-user-001 can read A/status + write A/command, DENY write A/status"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    acl = MosquittoACL()
    acl.parse(ACL_CONFIG)

    print("=" * 78)
    print("Mosquitto ACL Device Isolation Test (9 cases)")
    print("=" * 78)

    passed = 0
    failed = 0

    for name, username, topic, action, expected in TESTS:
        actual = acl.check(username, topic, action)
        ok = (actual == expected)
        status = "PASS" if ok else "FAIL"
        if ok:
            passed += 1
        else:
            failed += 1
        print(f"\n  [{status}] {name}")
        print(f"           user={username!r} topic={topic} action={action}")
        print(f"           expected={'ALLOW' if expected else 'DENY'} actual={'ALLOW' if actual else 'DENY'}")

    # Test 8
    print()
    ok8, msg8 = test_8_pwa_user_matrix(acl)
    status = "PASS" if ok8 else "FAIL"
    if ok8:
        passed += 1
    else:
        failed += 1
    print(f"  [{status}] 8. PWA user matrix (3-way check)")
    print(f"           {msg8}")

    print()
    print("=" * 78)
    print(f"Results: {passed}/{passed + failed} passed")
    print("=" * 78)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
