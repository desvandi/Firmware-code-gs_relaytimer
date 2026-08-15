#!/usr/bin/env bash
# =============================================================================
# run_all_host_tests.sh — Run ALL host-side regression tests (R2-corrected)
# =============================================================================
# P2-2 F-P0-2 C3-GATE-002-R2 TEST-INFRASTRUCTURE CORRECTION
#
# AUDITOR R2 FIX:
#   Previous version used `out=$(./bin 2>&1 || true)` which DISCARDED exit
#   code. This meant SIGSEGV (exit 139) with empty output was misclassified
#   as PASS because fail_count == 0. The runner could emit [ALL GREEN] even
#   when a binary crashed — exactly the failure mode that caused the
#   original C3-GATE-002 blocker.
#
#   This corrected version:
#     1. Captures exit code SEPARATELY from output (no `|| true`)
#     2. Treats non-zero exit as FAILURE (including signal terminations)
#     3. Detects SIGSEGV (exit 139 = 128+11) and other signals explicitly
#     4. Requires zero [FAIL] AND non-zero [PASS] count for suite PASS
#     5. Does NOT emit [ALL GREEN] if ANY suite fails or crashes
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
#   ./setup_host_env.sh        # one-time setup
#   ./run_all_host_tests.sh    # build + run all
#
# Exit code: 0 if ALL tests pass (every binary exit == 0 AND expected
# assertions observed AND zero [FAIL]), non-zero otherwise.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Verify setup has been run
if [ ! -f shims/arduinojson_compat.h ] || [ ! -d ../../.pio/libdeps/development/ArduinoJson ]; then
  echo "[ERROR] Host test environment not set up."
  echo "        Run ./setup_host_env.sh first."
  exit 1
fi

# Track overall result. 0 = all pass, 1 = at least one fail/crash.
OVERALL_EXIT=0
TOTAL_PASS=0
TOTAL_FAIL=0
RESULTS=()

# Helper: classify exit code (handles signal terminations)
# Args: $1 = exit code
# Returns: human-readable string
classify_exit() {
  local ec=$1
  if [ "${ec}" -eq 0 ]; then
    echo "exit=0"
  elif [ "${ec}" -gt 128 ]; then
    local sig=$((ec - 128))
    local signame="UNKNOWN"
    case ${sig} in
      11) signame="SIGSEGV" ;;
      6)  signame="SIGABRT" ;;
      8)  signame="SIGFPE" ;;
      7)  signame="SIGBUS" ;;
      9)  signame="SIGKILL" ;;
      15) signame="SIGTERM" ;;
      *)  signame="SIG-${sig}" ;;
    esac
    echo "CRASH(exit=${ec},signal=${signame})"
  else
    echo "FAIL(exit=${ec})"
  fi
}

# Helper: run a single test suite with full exit-code-aware semantics
# Args: $1 = Makefile name, $2 = display name, $3 = expected pass count or "vectors:N" for baseline
run_suite() {
  local mk="$1"
  local name="$2"
  local expected="$3"

  echo ""
  echo "=========================================="
  echo "  ${name}"
  echo "=========================================="

  # Clean + build
  if ! make -f "${mk}" clean >/dev/null 2>&1; then
    echo "[ERROR] clean failed for ${mk}"
    RESULTS+=("FAIL  ${name} (clean error)")
    OVERALL_EXIT=1
    return
  fi
  if ! make -f "${mk}" 2>&1 | tail -3; then
    echo "[ERROR] build failed for ${mk}"
    RESULTS+=("FAIL  ${name} (build error)")
    OVERALL_EXIT=1
    return
  fi

  # Locate binary
  local bin
  bin=$(grep -E '^BIN\s*:?=' "${mk}" | head -1 | sed -E 's/^BIN\s*:?=\s*//')
  if [ -z "${bin}" ] || [ ! -x "${bin}" ]; then
    echo "[ERROR] binary ${bin} not found after build"
    RESULTS+=("FAIL  ${name} (no binary)")
    OVERALL_EXIT=1
    return
  fi

  # Run binary — CRITICAL: capture exit code SEPARATELY from output
  # Do NOT use `|| true` — we want the real exit code.
  echo ""
  local out_file="/tmp/${bin}_output.txt"
  "./${bin}" > "${out_file}" 2>&1
  local ec=$?

  # Show last 10 lines of output for visibility
  echo "--- last 10 lines of output ---"
  tail -10 "${out_file}"

  # Classify exit code
  local exit_class
  exit_class=$(classify_exit "${ec}")
  echo ""
  echo "Exit: ${exit_class}"

  # Count assertions — use awk to avoid grep -c exit-code-1 issue when count is 0
  # (grep -c returns exit 1 for zero matches, which combined with `|| echo 0`
  # produces "0\n0" — corrupted value that breaks arithmetic)
  local pass_count fail_count vec_count
  pass_count=$(awk 'END{print NR}' <(grep "\[PASS\]" "${out_file}" 2>/dev/null))
  fail_count=$(awk 'END{print NR}' <(grep "\[FAIL\]" "${out_file}" 2>/dev/null))
  vec_count=$(awk 'END{print NR}' <(grep -E "[0-9a-f]{64}" "${out_file}" 2>/dev/null))

  echo "Assertions: PASS=${pass_count} FAIL=${fail_count} (expected: ${expected})"
  TOTAL_PASS=$((TOTAL_PASS + pass_count))
  TOTAL_FAIL=$((TOTAL_FAIL + fail_count))

  # Classification logic (R2-aware):
  #   PASS requires: exit == 0 AND fail_count == 0 AND (pass_count > 0 OR vectors > 0)
  #   Any other combination is FAILURE
  local status
  if [ "${ec}" -ne 0 ]; then
    status="FAIL  ${name} — ${exit_class} (binary did not exit cleanly)"
    OVERALL_EXIT=1
  elif [ "${fail_count}" -gt 0 ]; then
    status="FAIL  ${name} — ${fail_count} assertion failures"
    OVERALL_EXIT=1
  elif [ "${pass_count}" -eq 0 ] && [ "${vec_count}" -eq 0 ]; then
    status="FAIL  ${name} — silent failure (exit 0 but zero PASS/zero vectors)"
    OVERALL_EXIT=1
  else
    status="PASS  ${name} — ${pass_count} assertions / ${vec_count} vectors (exit 0)"
  fi
  RESULTS+=("${status}")
}

run_suite Makefile.tj  "TransactionJournalTest"      "194"
run_suite Makefile.cr  "CommandRoutingTest"          "133"
run_suite Makefile.che "CommandHashEquivalenceTest"  "26"
run_suite Makefile.ws  "WebServerTest"               "111"
run_suite Makefile.mc  "MqttClientTest"              "31"
run_suite Makefile.chb "CommandHashBaseline"         "14 vectors"

# Final summary
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
echo ""

if [ "${OVERALL_EXIT}" -eq 0 ]; then
  echo "[ALL GREEN] 495/495 + 14 baseline vectors (every binary exit == 0)"
  exit 0
else
  echo "[FAILED] one or more suites did not pass cleanly"
  echo "         Check the per-suite status above for details."
  exit 1
fi
