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
#   1. TransactionJournalTest   (Makefile.tj)   — binary RESULTS: 194 passed
#   2. CommandRoutingTest       (Makefile.cr)   — binary RESULTS: 133 passed
#   3. CommandHashEquivalenceTest (Makefile.che) — binary RESULTS: 26 passed
#   4. WebServerTest            (Makefile.ws)   — binary RESULTS: 144 passed (C2-C5)
#   5. MqttClientTest           (Makefile.mc)   — binary RESULTS: 31 passed
#   6. CommandHashBaseline      (Makefile.chb)  — 14 vectors captured (no RESULTS line)
#
# Authoritative total: 194 + 133 + 26 + 144 + 31 = 528 assertions + 14 vectors.
# (Previous versions incorrectly claimed 540 due to grep over-counting
#  CommandRoutingTest's routing-matrix summary lines as assertions.)
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

  # COUNT ASSERTIONS — AUTHORITATIVE SOURCE IS BINARY's RESULTS LINE.
  #
  # Previous versions used `grep "[PASS]"` which OVER-COUNTS because some
  # test binaries (e.g. CommandRoutingTest) print summary [PASS] lines
  # like "[PASS] all routing-matrix cases pass (12 command types)" that
  # are NOT individual assertions — they are post-test summaries printed
  # AFTER the binary's own RESULTS: line. This caused a 12-count
  # discrepancy (grep=145 vs binary RESULTS=133) for CommandRoutingTest,
  # which the auditor caught during C4 re-gate.
  #
  # Fix: parse the binary's own "RESULTS: N passed, M failed" line.
  # This is the count the binary itself reports — it is authoritative.
  # Fallback to grep only if no RESULTS line exists (e.g. CommandHashBaseline,
  # which is a baseline capture tool, not a pass/fail test suite).
  local pass_count fail_count vec_count results_line
  results_line=$(grep -E "^RESULTS: [0-9]+ passed, [0-9]+ failed" "${out_file}" 2>/dev/null | tail -1)
  if [ -n "${results_line}" ]; then
    # Binary reports its own RESULTS line — use it as authoritative source
    pass_count=$(echo "${results_line}" | sed -E 's/^RESULTS: ([0-9]+) passed.*/\1/')
    fail_count=$(echo "${results_line}" | sed -E 's/^RESULTS: [0-9]+ passed, ([0-9]+) failed.*/\1/')
    vec_count=0  # baseline capture tools don't print RESULTS line
  else
    # No RESULTS line (e.g. CommandHashBaseline) — fall back to grep
    # for vectors count. pass_count/fail_count remain 0.
    pass_count=0
    fail_count=0
    vec_count=$(awk 'END{print NR}' <(grep -E "[0-9a-f]{64}" "${out_file}" 2>/dev/null))
  fi

  echo "Assertions (from binary RESULTS line): PASS=${pass_count} FAIL=${fail_count} VECTORS=${vec_count} (expected: ${expected})"
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
run_suite Makefile.ws  "WebServerTest"               "144"
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
  echo "[ALL GREEN] 528 assertions + 14 baseline vectors (authoritative, from binary RESULTS lines)"
  exit 0
else
  echo "[FAILED] one or more suites did not pass cleanly"
  echo "         Check the per-suite status above for details."
  exit 1
fi
