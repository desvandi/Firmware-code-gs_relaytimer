#!/usr/bin/env bash
# =============================================================================
# run_host_tests.sh — convenience wrapper for all host-side tests.
# Runs both Phase 1 (JournalRecord) and Phase 2 (TransactionJournal) test
# harnesses. Exits with non-zero code if ANY test fails.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_DIR="${SCRIPT_DIR}/host"

cd "${HOST_DIR}"

echo "=========================================="
echo "Phase 1: JournalRecord host test"
echo "=========================================="
make clean >/dev/null 2>&1 || true
make run-journal_record_test 2>/dev/null || make run 2>/dev/null || {
  # Fallback: run journal_record_test directly if Makefile target naming differs
  make -f Makefile 2>&1
  ./journal_record_test
}

echo ""
echo "=========================================="
echo "Phase 2 P2-1: TransactionJournal host test"
echo "=========================================="
make -f Makefile.tj clean >/dev/null 2>&1 || true
make -f Makefile.tj run
