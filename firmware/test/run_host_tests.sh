#!/usr/bin/env bash
# =============================================================================
# run_host_tests.sh — convenience wrapper for the host-side Phase 1 test.
# Builds (if needed) and runs the test harness. Exits with the test's exit
# code so it can be used as a CI gate.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_DIR="${SCRIPT_DIR}/host"

cd "${HOST_DIR}"
make clean >/dev/null 2>&1 || true
make run
