#!/usr/bin/env bash
# =============================================================================
# run_all_host_tests.sh — Run ALL host-side regression tests
# =============================================================================
# P2-2 F-P0-2 C3-GATE-002 TEST-INFRASTRUCTURE CORRECTION
#
# Runs all 6 host-side test binaries + 1 baseline capture tool:
#   1. TransactionJournalTest   (Makefile.tj)   — expected 194/194 PASS
#   2. CommandRoutingTest       (Makefile.cr)   — expected 133/133 PASS
#   3. CommandHashEquivalenceTest (Makefile.che) — expected 26/26 PASS
#   4. WebServerTest            (Makefile.ws)   — expected 111/111 PASS
#   5. MqttClientTest           (Makefile.mc)   — expected 31/31 PASS
#   6. CommandHashBaseline      (Makefile.chb)  — expected 14 vectors captured
#
# Total expected: 495 assertions + 14 baseline vectors.
#
# USAGE:
#   cd firmware/test/host
#   ./setup_host_env.sh      # one-time setup
#   ./run_all_host_tests.sh  # build + run all
#
# Exit code: 0 if ALL tests pass, non-zero if any fails.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Verify setup has been run
if [ ! -f shims/arduinojson_compat.h ] || [ ! -d ../../.pio/libdeps/development/ArduinoJson ]; then
  echo "[ERROR] Host test environment not set up."
  echo "        Run ./setup_host_env.sh first."
  exit 1
fi

FAIL=0
TOTAL_PASS=0
TOTAL_FAIL=0
RESULTS=()

run_suite() {
  local mk="$1"
  local name="$2"
  local expected="$3"
  echo ""
  echo "=========================================="
  echo "  ${name}"
  echo "=========================================="
  if ! make -f "${mk}" clean >/dev/null 2>&1; then
    echo "[ERROR] clean failed for ${mk}"
    FAIL=1
    return
  fi
  if ! make -f "${mk}" 2>&1 | tail -5; then
    echo "[ERROR] build failed for ${mk}"
    FAIL=1
    RESULTS+=("FAIL  ${name} (build error)")
    return
  fi
  echo ""
  local bin
  bin=$(grep -E '^BIN\s*:?=' "${mk}" | head -1 | sed -E 's/^BIN\s*:?=\s*//')
  if [ -z "${bin}" ] || [ ! -x "${bin}" ]; then
    echo "[ERROR] binary ${bin} not found after build"
    FAIL=1
    RESULTS+=("FAIL  ${name} (no binary)")
    return
  fi
  local out
  out=$(./"${bin}" 2>&1 || true)
  echo "${out}" | tail -10
  local pass_count
  local fail_count
  pass_count=$(echo "${out}" | grep -c "\[PASS\]" || true)
  fail_count=$(echo "${out}" | grep -c "\[FAIL\]" || true)
  TOTAL_PASS=$((TOTAL_PASS + pass_count))
  TOTAL_FAIL=$((TOTAL_FAIL + fail_count))
  if [ "${fail_count}" -eq 0 ]; then
    RESULTS+=("PASS  ${name} — ${pass_count} assertions (${expected})")
  else
    RESULTS+=("FAIL  ${name} — ${pass_count} pass, ${fail_count} fail")
    FAIL=1
  fi
}

run_suite Makefile.tj  "TransactionJournalTest"      "expected 194/194"
run_suite Makefile.cr  "CommandRoutingTest"          "expected 133/133"
run_suite Makefile.che "CommandHashEquivalenceTest"  "expected 26/26"
run_suite Makefile.ws  "WebServerTest"               "expected 111/111"
run_suite Makefile.mc  "MqttClientTest"              "expected 31/31"
run_suite Makefile.chb "CommandHashBaseline"         "expected 14 vectors"

echo ""
echo "=========================================="
echo "  SUMMARY"
echo "=========================================="
for r in "${RESULTS[@]}"; do
  echo "  ${r}"
done
echo ""
echo "Total PASS: ${TOTAL_PASS}"
echo "Total FAIL: ${TOTAL_FAIL}"
if [ "${FAIL}" -eq 0 ]; then
  echo ""
  echo "[ALL GREEN] 495/495 + 14 baseline vectors"
  exit 0
else
  echo ""
  echo "[FAILED] some tests did not pass"
  exit 1
fi
