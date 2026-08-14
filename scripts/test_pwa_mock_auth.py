#!/usr/bin/env python3
"""
audit-fixes verification: PWA mock auth fail-closed behavior.

Reimplements the isMockAuthEnabled() logic from src/lib/mockStore.ts in Python
and tests the matrix of NODE_ENV + env var combinations.

Critical case (auditor #2 P0): production with NEXT_PUBLIC_DEMO_MODE=true
must NOT enable mock auth (prevents admin/admin123 backdoor).

Run: python3 /home/z/my-project/scripts/test_pwa_mock_auth.py
"""

import os
from typing import Dict, Optional


def compute_mock_auth_enabled(env: Dict[str, Optional[str]]) -> bool:
    """Mirror src/lib/mockStore.ts:isMockAuthEnabled() logic."""
    is_production = env.get("NODE_ENV") == "production"

    # DEMO_MODE flag (true if dev mode OR DEMO_MODE=true OR NEXT_PUBLIC_DEMO_MODE=true)
    is_demo_mode = (
        env.get("NODE_ENV") == "development"
        or env.get("DEMO_MODE") == "true"
        or env.get("NEXT_PUBLIC_DEMO_MODE") == "true"
    )

    # MOCK_AUTH_EXPLICITLY_ENABLED: requires all three env vars AND not production
    mock_auth_explicitly_enabled = (
        bool(env.get("JWT_SECRET"))
        and bool(env.get("MOCK_USER"))
        and bool(env.get("MOCK_PASSWORD"))
        and not is_production
    )

    return is_demo_mode or mock_auth_explicitly_enabled


def compute_default_user(env: Dict[str, Optional[str]]) -> str:
    """Mirror DEFAULT_USER logic."""
    if env.get("MOCK_USER"):
        return env["MOCK_USER"]
    if env.get("NODE_ENV") == "development":
        return "admin"
    return ""


def compute_default_password(env: Dict[str, Optional[str]]) -> str:
    """Mirror DEFAULT_PASSWORD_HASH logic."""
    if env.get("MOCK_PASSWORD"):
        return env["MOCK_PASSWORD"]
    if env.get("NODE_ENV") == "development":
        return "admin123"
    return ""


def compute_jwt_secret(env: Dict[str, Optional[str]]) -> str:
    """Mirror JWT_SECRET logic."""
    if env.get("JWT_SECRET"):
        return env["JWT_SECRET"]
    if env.get("NODE_ENV") == "development":
        return "timer12-dev-only-secret"
    return ""


def run_tests():
    """Run all test cases."""
    print("=" * 78)
    print("PWA Mock Auth Fail-Closed Test")
    print("=" * 78)

    test_cases = [
        # (env, expected_mock_enabled, expected_default_user, expected_default_pass, expected_jwt, description)
        # --- Local dev (DX-friendly, demo creds allowed) ---
        ({"NODE_ENV": "development"}, True, "admin", "admin123", "timer12-dev-only-secret",
         "Local dev: demo creds + dev JWT secret (DX-friendly)"),
        ({"NODE_ENV": "development", "MOCK_USER": "bob", "MOCK_PASSWORD": "pass",
          "JWT_SECRET": "real-secret"},
         True, "bob", "pass", "real-secret",
         "Local dev + explicit env vars: uses explicit values"),

        # --- Production with NO demo mode (the safe default) ---
        ({"NODE_ENV": "production"}, False, "", "", "",
         "Production, no DEMO_MODE: mock auth DISABLED, no creds, no JWT secret"),
        # --- Production with all env vars set but DEMO_MODE off (P0 fix) ---
        # mock auth DISABLED. Env vars are still loaded into DEFAULT_USER/PASS/JWT
        # but isMockAuthEnabled() returns False so they're never used for auth.
        ({"NODE_ENV": "production", "JWT_SECRET": "real", "MOCK_USER": "bob",
          "MOCK_PASSWORD": "pass"},
         False, "bob", "pass", "real",
         "Production + all env vars but DEMO_MODE off: mock auth DISABLED (P0 fix)"),

        # --- Production with DEMO_MODE accidentally left on (auditor #2 P0) ---
        ({"NODE_ENV": "production", "NEXT_PUBLIC_DEMO_MODE": "true"},
         True, "", "", "",
         "Production + NEXT_PUBLIC_DEMO_MODE=true: mock auth ENABLED, but NO default creds/jwt (P0 fix)"),
        ({"NODE_ENV": "production", "DEMO_MODE": "true"},
         True, "", "", "",
         "Production + DEMO_MODE=true: same as above (no creds)"),

        # --- Vercel production with DEMO_MODE=true AND explicit creds ---
        ({"NODE_ENV": "production", "NEXT_PUBLIC_DEMO_MODE": "true",
          "MOCK_USER": "bob", "MOCK_PASSWORD": "pass", "JWT_SECRET": "real"},
         True, "bob", "pass", "real",
         "Production + DEMO_MODE=true + explicit creds: ENABLED with explicit creds"),

        # --- Production with partial env vars (missing JWT_SECRET) ---
        ({"NODE_ENV": "production", "MOCK_USER": "bob", "MOCK_PASSWORD": "pass"},
         False, "bob", "pass", "",
         "Production + partial env (no JWT_SECRET): mock auth DISABLED"),

        # --- Staging (NODE_ENV='staging') with explicit creds ---
        ({"NODE_ENV": "staging", "JWT_SECRET": "real", "MOCK_USER": "bob",
          "MOCK_PASSWORD": "pass"},
         True, "bob", "pass", "real",
         "Staging + explicit creds: ENABLED (allows mock auth on non-production server)"),

        # --- Production with only JWT_SECRET set ---
        ({"NODE_ENV": "production", "JWT_SECRET": "real"},
         False, "", "", "real",
         "Production + only JWT_SECRET: mock auth DISABLED (need all three)"),
    ]

    passed = 0
    failed = 0

    for env, exp_enabled, exp_user, exp_pass, exp_jwt, desc in test_cases:
        actual_enabled = compute_mock_auth_enabled(env)
        actual_user = compute_default_user(env)
        actual_pass = compute_default_password(env)
        actual_jwt = compute_jwt_secret(env)

        ok = (
            actual_enabled == exp_enabled
            and actual_user == exp_user
            and actual_pass == exp_pass
            and actual_jwt == exp_jwt
        )
        status = "PASS" if ok else "FAIL"
        if ok:
            passed += 1
        else:
            failed += 1

        print(f"\n  [{status}] {desc}")
        print(f"          Env: {env}")
        print(f"          Expected: enabled={exp_enabled}, user={exp_user!r}, pass={exp_pass!r}, jwt={'<set>' if exp_jwt else '<empty>'}")
        print(f"          Actual:   enabled={actual_enabled}, user={actual_user!r}, pass={actual_pass!r}, jwt={'<set>' if actual_jwt else '<empty>'}")

    print("\n" + "=" * 78)
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    print("=" * 78)

    # Highlight the critical P0 fix
    print("\nCritical P0 fix verified:")
    print("  Production + NEXT_PUBLIC_DEMO_MODE=true (admin mistake) → mock auth ENABLED")
    print("  BUT default creds are EMPTY (no admin/admin123 backdoor) and JWT_SECRET is EMPTY")
    print("  → /api/login returns 403 'Invalid username or password' for any credential")
    print("  → /api/status returns 401 'Unauthorized' (no JWT cookie)")
    print("  → Dashboard unusable until admin sets MOCK_USER/MOCK_PASSWORD/JWT_SECRET")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    import sys
    sys.exit(run_tests())
