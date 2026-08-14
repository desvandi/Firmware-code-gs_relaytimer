#!/usr/bin/env python3
"""
audit-fixes verification: OTA URL host allowlist security test.

Reimplements the exact logic from MqttClient.cpp::_handleOta() (OTA URL
allowlist section) in Python, then runs the 6 bypass cases the engineer
requested plus additional edge cases.

The C++ logic (from firmware/MqttClient.cpp):
  1. Require HTTPS (reject http://)
  2. Extract host from URL: skip 'https://', read until '/', ':', '?', or end
  3. Lowercase host
  4. For each comma-separated entry in OTA_ALLOWED_HOSTS:
       - Trim whitespace
       - Suffix match: host == entry OR (host ends with '.' + entry)
  5. If no match -> reject
  6. PRODUCTION_BUILD: if OTA_ALLOWED_HOSTS empty -> reject (fail-closed)

Run: python3 /home/z/my-project/scripts/test_ota_allowlist.py
"""


def extract_host(url: str) -> str:
    """Mirror C++ host extraction: skip 'https://', read until '/', ':', '?', '#', or end."""
    if not url.startswith("https://") and not url.startswith("http://"):
        return ""
    scheme_end = url.find("://") + 3
    rest = url[scheme_end:]
    end = len(rest)
    for i, c in enumerate(rest):
        if c in "/:?#":
            end = i
            break
    return rest[:end].lower()


def is_host_allowed(url: str, allowed_hosts: str) -> bool:
    """Mirror C++ allowlist logic. Returns True if allowed, False if rejected."""
    if not allowed_hosts:
        return True
    host = extract_host(url)
    if not host:
        return False
    allowed_list = [a.strip().lower() for a in allowed_hosts.split(",") if a.strip()]
    for entry in allowed_list:
        if host == entry:
            return True
        if host.endswith("." + entry) and len(host) > len(entry) + 1:
            return True
    return False


def combined_check(url: str, allowed_hosts: str, production: bool = False) -> str:
    """Mirror full C++ flow: HTTPS check + allowlist + production fail-closed."""
    if not url.startswith("https://"):
        return "REJECT (not HTTPS)"
    if production and not allowed_hosts:
        return "REJECT (production requires OTA_ALLOWED_HOSTS)"
    if allowed_hosts:
        if not is_host_allowed(url, allowed_hosts):
            return "REJECT (host not in allowlist)"
    return "ACCEPT"


def run_tests():
    ALLOWED = "github.com,raw.githubusercontent.com,updates.example.com"
    print("=" * 78)
    print("OTA URL Host Allowlist Security Test")
    print(f"Allowlist: {ALLOWED}")
    print("=" * 78)

    test_cases = [
        ("https://github.com/releases/download/v1/firmware.bin",
         "ACCEPT", "Exact match: github.com (allowed)"),
        ("https://evil.example/firmware.bin",
         "REJECT", "Different domain (evil.example not in allowlist)"),
        ("https://github.com.evil.com/firmware.bin",
         "REJECT", "Tricky: host is 'github.com.evil.com' - should NOT match github.com"),
        ("https://evil.com/@github.com/firmware.bin",
         "REJECT", "Tricky: github.com appears in path, not host"),
        ("https://github.com:443/releases/firmware.bin",
         "ACCEPT", "Port 443 (default HTTPS port) - host extraction stops at ':'"),
        ("http://github.com/firmware.bin",
         "REJECT", "Plain HTTP - rejected before allowlist check"),
        ("https://raw.githubusercontent.com/firmware.bin",
         "ACCEPT", "Exact match: raw.githubusercontent.com"),
        ("https://api.github.com/firmware.bin",
         "ACCEPT", "Subdomain of github.com (suffix match - subdomains allowed)"),
        ("https://updates.example.com/firmware.bin",
         "ACCEPT", "Exact match: updates.example.com"),
        ("https://staging.updates.example.com/firmware.bin",
         "ACCEPT", "Subdomain of updates.example.com (suffix match)"),
        ("https://fakeupdates.example.com/firmware.bin",
         "REJECT", "NOT a subdomain of updates.example.com (no dot prefix)"),
        ("https://evilgithub.com/firmware.bin",
         "REJECT", "Host ends with 'github.com' substring but no dot - should NOT match"),
        ("https://GitHub.COM/firmware.bin",
         "ACCEPT", "Case-insensitive: GitHub.COM should match github.com"),
        ("https://GITHUB.COM/firmware.bin",
         "ACCEPT", "All-uppercase host"),
        ("https://github.com.evil.com:443/firmware.bin",
         "REJECT", "Tricky + port: should still reject"),
        ("https://github.com.evil.com@github.com/firmware.bin",
         "REJECT", "URL with userinfo - host is 'github.com.evil.com' (parsed before @)"),
        ("https://github.com/firmware.bin?param=evil.com",
         "ACCEPT", "Query string after path - host is still github.com"),
        ("https://github.com#fragment",
         "ACCEPT", "Fragment - host is still github.com"),
        ("https://192.168.1.1/firmware.bin",
         "REJECT", "IP address - not in allowlist"),
        ("https://github.com",
         "ACCEPT", "No path - host is github.com"),
        ("https://github.com/",
         "ACCEPT", "Root path - host is github.com"),
    ]

    prod_cases = [
        ("https://github.com/firmware.bin", "REJECT", "PRODUCTION_BUILD with empty allowlist should reject"),
        ("https://evil.example/firmware.bin", "REJECT", "Same - any URL rejected"),
        ("http://github.com/firmware.bin", "REJECT", "HTTP also rejected (not HTTPS)"),
    ]

    passed = 0
    failed = 0

    print("\n--- Dev mode tests (allowlist configured) ---")
    for url, expected, desc in test_cases:
        result = combined_check(url, ALLOWED, production=False)
        ok = result.startswith(expected)
        status = "PASS" if ok else "FAIL"
        if ok:
            passed += 1
        else:
            failed += 1
        print(f"  [{status}] {desc}")
        print(f"          URL: {url}")
        print(f"          Expected: {expected} | Got: {result}")

    print("\n--- Production mode tests (allowlist configured) ---")
    for url, expected, desc in test_cases:
        result = combined_check(url, ALLOWED, production=True)
        ok = result.startswith(expected)
        status = "PASS" if ok else "FAIL"
        if ok:
            passed += 1
        else:
            failed += 1
        print(f"  [{status}] {desc}")
        print(f"          URL: {url}")
        print(f"          Expected: {expected} | Got: {result}")

    print("\n--- Production mode with EMPTY allowlist (fail-closed) ---")
    for url, expected, desc in prod_cases:
        result = combined_check(url, "", production=True)
        ok = result.startswith(expected)
        status = "PASS" if ok else "FAIL"
        if ok:
            passed += 1
        else:
            failed += 1
        print(f"  [{status}] {desc}")
        print(f"          URL: {url}")
        print(f"          Expected: {expected} | Got: {result}")

    dev_empty_cases = [
        ("https://evil.example/firmware.bin", "ACCEPT", "Dev mode + empty allowlist = allowlist disabled"),
        ("https://github.com/firmware.bin", "ACCEPT", "Dev mode + empty allowlist = allowlist disabled"),
    ]
    print("\n--- Dev mode with empty allowlist (backward-compat) ---")
    for url, expected, desc in dev_empty_cases:
        result = combined_check(url, "", production=False)
        ok = result.startswith(expected)
        status = "PASS" if ok else "FAIL"
        if ok:
            passed += 1
        else:
            failed += 1
        print(f"  [{status}] {desc}")
        print(f"          URL: {url}")
        print(f"          Expected: {expected} | Got: {result}")

    print("\n" + "=" * 78)
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    print("=" * 78)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    import sys
    sys.exit(run_tests())
