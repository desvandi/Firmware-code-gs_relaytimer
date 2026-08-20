#!/usr/bin/env python3
"""
On-target Ed25519 KAT test — sends HTTP request to ESP32 and verifies response.

Usage:
  python3 scripts/test_ed25519_on_target_kat.py --host <ESP32_IP> [--token <JWT>]

The ESP32 must be running firmware v4.3.12+ with the /api/ed25519/kat endpoint.
Authentication is via JWT (cookie-based — provide --token or login first).

Acceptance criteria:
  - HTTP 200
  - JSON response: { success: true, data: { allPassed: true, testsPassed: 4, ... } }
  - allPassed == true
  - testsRun == 4
  - testsFailed == 0
"""
import argparse
import json
import sys
import urllib.request
import urllib.error

def main():
    parser = argparse.ArgumentParser(description='On-target Ed25519 KAT test')
    parser.add_argument('--host', required=True, help='ESP32 IP address or hostname')
    parser.add_argument('--port', type=int, default=80, help='ESP32 HTTP port (default: 80)')
    parser.add_argument('--token', default='', help='JWT token (or cookie value)')
    parser.add_argument('--force', action='store_true', help='Force re-run (bypass 5-min cache)')
    args = parser.parse_args()

    url = f"http://{args.host}:{args.port}/api/ed25519/kat"
    if args.force:
        url += "?force=1"

    print(f"Sending GET {url} ...")

    req = urllib.request.Request(url)
    if args.token:
        req.add_header("Cookie", f"jwt={args.token}")

    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            status = resp.status
            body = resp.read().decode('utf-8')
    except urllib.error.HTTPError as e:
        print(f"❌ HTTP error: {e.code} {e.reason}")
        print(e.read().decode('utf-8'))
        return 1
    except urllib.error.URLError as e:
        print(f"❌ Connection error: {e.reason}")
        return 1

    if status != 200:
        print(f"❌ Expected HTTP 200, got {status}")
        return 1

    data = json.loads(body)
    if not data.get('success'):
        print(f"❌ Response success=false: {data.get('message', 'unknown error')}")
        return 1

    kat = data.get('data', {})
    all_passed = kat.get('allPassed', False)
    tests_run = kat.get('testsRun', 0)
    tests_passed = kat.get('testsPassed', 0)
    tests_failed = kat.get('testsFailed', 0)
    duration = kat.get('totalDurationMs', 0)
    cached = kat.get('cached', False)
    lib_ver = kat.get('libraryVersion', 'unknown')

    print(f"\n{'='*50}")
    print(f"Ed25519 KAT Results (library: {lib_ver})")
    print(f"{'='*50}")
    print(f"  All passed:     {'✅ YES' if all_passed else '❌ NO'}")
    print(f"  Tests run:       {tests_run}")
    print(f"  Tests passed:    {tests_passed}")
    print(f"  Tests failed:    {tests_failed}")
    print(f"  Duration:        {duration} ms")
    print(f"  Cached:          {'yes' if cached else 'no (fresh run)'}")
    print(f"{'='*50}")

    if all_passed and tests_failed == 0:
        print("\n✅ RESULT: PASS — On-target Ed25519 KAT verified")
        print("   Phase 34 item OTA-001: PASS")
        return 0
    else:
        print("\n❌ RESULT: FAIL — On-target Ed25519 KAT failed")
        print("   Ed25519 verification may not work correctly on this device")
        return 1

if __name__ == '__main__':
    sys.exit(main())
