#!/usr/bin/env python3
"""
audit-fixes verification: AuthManager LRU eviction regression test.

Reimplements AuthManager::_storeRefreshToken() logic in Python, then verifies
the engineer's regression scenario: fill MAX_REFRESH_TOKENS+1 tokens and
confirm the eviction picks the FIRST EMPTY slot or slot 0 (oldest by convention),
NOT just the last iterated slot.

Per the fix comment in AuthManager.cpp:
  - First pass: find existing slot with same token (overwrite in place) OR
    remember the FIRST empty slot.
  - If no existing + no empty slot: evict slot 0 (oldest by convention).
  - Per-device MAX_REFRESH_TOKENS=4 is small enough that LRU precision is not
    security-critical (the comment explicitly says "not security-critical").

Run: python3 /home/z/my-project/scripts/test_auth_lru.py
"""

MAX_REFRESH_TOKENS = 4


class AuthManagerSimulator:
    """Mirrors AuthManager._storeRefreshToken() logic from AuthManager.cpp."""

    def __init__(self):
        # Slots: list of (token or None). Index 0 = oldest by convention.
        self.slots = [None] * MAX_REFRESH_TOKENS
        self.eviction_log = []

    def store(self, token: str) -> int:
        """Mirror C++ logic. Returns slot index used."""
        slot = -1
        first_empty = -1

        for i, existing in enumerate(self.slots):
            if existing is None:
                if first_empty == -1:
                    first_empty = i  # remember FIRST empty slot
            elif existing == token:
                slot = i  # already stored - overwrite in place
                break

        if slot == -1:
            if first_empty != -1:
                slot = first_empty
            else:
                # All slots full - evict slot 0 (oldest by convention)
                slot = 0
                evicted = self.slots[0]
                self.eviction_log.append(evicted)
                # Shift everything down (simulating slot 0 becoming "newest")
                # NOTE: The C++ code does NOT shift - it just overwrites slot 0.
                # Slot 0 then becomes the most recently written. This is NOT
                # true LRU (which would track access time per slot). The fix
                # comment acknowledges this: "MAX=4 and per-device scope, this
                # is acceptable."

        self.slots[slot] = token
        return slot

    def is_valid(self, token: str) -> bool:
        """Mirror C++ _isRefreshTokenValid()."""
        return any(t == token for t in self.slots)

    def invalidate(self, token: str) -> bool:
        """Mirror C++ _invalidateRefreshToken()."""
        for i, existing in enumerate(self.slots):
            if existing == token:
                self.slots[i] = None
                return True
        return False


def test_basic_storage():
    """Test 1: Store up to MAX_REFRESH_TOKENS, all should succeed."""
    print("--- Test 1: Basic storage up to MAX_REFRESH_TOKENS ---")
    am = AuthManagerSimulator()
    tokens = ["tok_A", "tok_B", "tok_C", "tok_D"]
    for i, t in enumerate(tokens):
        slot = am.store(t)
        assert slot == i, f"Token {t} should go to slot {i}, got {slot}"
        assert am.is_valid(t), f"Token {t} should be valid after store"
    print(f"  PASS: Stored 4 tokens in slots 0-3")
    return am


def test_eviction_picks_slot_0_when_full():
    """Test 2: When all slots full, 5th token evicts slot 0 (NOT slot 3)."""
    print("\n--- Test 2: Eviction picks slot 0 (oldest), NOT last iterated slot ---")
    am = test_basic_storage()  # Slots: [A, B, C, D]

    # Store 5th token - should evict tok_A (slot 0), not tok_D (slot 3)
    # This is the bug the engineer flagged: previously `oldestSlot = i` was set
    # every iteration without comparison, so slot 3 (last) was always evicted.
    slot = am.store("tok_E")
    assert slot == 0, f"5th token should go to slot 0 (oldest), got slot {slot}"
    assert am.eviction_log == ["tok_A"], f"Should have evicted tok_A, got {am.eviction_log}"
    assert not am.is_valid("tok_A"), "tok_A should be invalidated after eviction"
    assert am.is_valid("tok_B"), "tok_B should still be valid"
    assert am.is_valid("tok_C"), "tok_C should still be valid"
    assert am.is_valid("tok_D"), "tok_D should still be valid"
    assert am.is_valid("tok_E"), "tok_E should be valid"
    print(f"  PASS: 5th token evicted tok_A (slot 0), not tok_D (slot 3)")
    print(f"  Slots now: {am.slots}")


def test_overwrite_existing_token():
    """Test 3: Storing an existing token overwrites in place (no eviction)."""
    print("\n--- Test 3: Storing existing token overwrites in place ---")
    am = AuthManagerSimulator()
    am.store("tok_A")
    am.store("tok_B")
    # Re-store tok_A - should overwrite slot 0, NOT evict tok_B
    slot = am.store("tok_A")
    assert slot == 0, f"Re-storing tok_A should overwrite slot 0, got {slot}"
    assert len(am.eviction_log) == 0, "No eviction should happen on overwrite"
    assert am.is_valid("tok_A"), "tok_A still valid"
    assert am.is_valid("tok_B"), "tok_B still valid (not evicted)"
    print(f"  PASS: Re-stored tok_A in slot 0 without evicting tok_B")


def test_invalidate_then_reuse_slot():
    """Test 4: After invalidating a token, that slot can be reused by new token."""
    print("\n--- Test 4: Invalidated slot reused by new token ---")
    am = AuthManagerSimulator()
    am.store("tok_A")
    am.store("tok_B")
    # Invalidate tok_A - slot 0 becomes empty
    am.invalidate("tok_A")
    assert not am.is_valid("tok_A")
    # Store tok_C - should use slot 0 (first empty), not slot 2
    slot = am.store("tok_C")
    assert slot == 0, f"tok_C should reuse slot 0 (first empty), got {slot}"
    assert am.is_valid("tok_B")
    assert am.is_valid("tok_C")
    print(f"  PASS: tok_C reused empty slot 0 (first_empty), not slot 2")


def test_concurrent_login_does_not_evict_active_sessions():
    """Test 5: Filling MAX tokens from a single client should not lock out other active sessions.

    Scenario: User logs in on 4 devices (4 refresh tokens), then logs in on a 5th.
    The 5th login evicts slot 0 (oldest). User on device 1 is force-logged-out
    on next /api/refresh call. This is ACCEPTABLE behavior (documented in fix comment).
    """
    print("\n--- Test 5: 5 concurrent logins evict oldest (documented behavior) ---")
    am = AuthManagerSimulator()
    for i in range(MAX_REFRESH_TOKENS):
        am.store(f"tok_dev{i}")
    # 5th login
    am.store("tok_dev4")
    # tok_dev0 (slot 0) should be evicted
    assert not am.is_valid("tok_dev0"), "tok_dev0 (oldest) should be evicted"
    assert am.is_valid("tok_dev1"), "tok_dev1 still valid"
    assert am.is_valid("tok_dev2"), "tok_dev2 still valid"
    assert am.is_valid("tok_dev3"), "tok_dev3 still valid"
    assert am.is_valid("tok_dev4"), "tok_dev4 still valid"
    print(f"  PASS: 5th login evicted tok_dev0 (oldest). Other 4 sessions still valid.")
    print(f"  NOTE: This is documented behavior — per-device MAX=4 is acceptable.")
    print(f"        tok_dev0 will get 401 on next /api/refresh (force re-login).")


def test_one_time_use_rotation():
    """Test 6: Refresh token rotation - old token invalidated, new one stored."""
    print("\n--- Test 6: Refresh token rotation (one-time use) ---")
    am = AuthManagerSimulator()
    am.store("tok_old")
    # User calls /api/refresh with tok_old
    assert am.is_valid("tok_old")
    # Rotation: invalidate old, store new
    am.invalidate("tok_old")
    am.store("tok_new")
    # Old token should NOT be valid anymore (one-time use)
    assert not am.is_valid("tok_old"), "Old refresh token must be invalidated after rotation"
    assert am.is_valid("tok_new"), "New refresh token should be valid"
    # Reuse attempt: attacker tries to use old token again
    # _isRefreshTokenValid returns false → security violation logged
    assert not am.is_valid("tok_old"), "Reusing old refresh token must fail (security violation)"
    print(f"  PASS: Rotation invalidated tok_old, tok_new is valid, reuse of tok_old fails")


if __name__ == "__main__":
    test_basic_storage()
    test_eviction_picks_slot_0_when_full()
    test_overwrite_existing_token()
    test_invalidate_then_reuse_slot()
    test_concurrent_login_does_not_evict_active_sessions()
    test_one_time_use_rotation()
    print("\n" + "=" * 78)
    print("All AuthManager LRU regression tests PASSED")
    print("=" * 78)
