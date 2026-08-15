#!/usr/bin/env bash
# =============================================================================
# setup_host_env.sh — Initialize host-test build environment
# =============================================================================
# P2-2 F-P0-2 C3-GATE-002-R1 TEST-INFRASTRUCTURE CORRECTION (revised)
#
# PURPOSE:
#   The host-side test Makefiles (Makefile.ws / Makefile.mc / Makefile.che /
#   Makefile.chb) compile production firmware source against minimal host
#   shims under firmware/test/host/shims/. Two host-environment dependencies
#   must be satisfied before the host tests will build and run cleanly.
#
#     1. ArduinoJson v6.19.1+ must be installed at
#        firmware/.pio/libdeps/development/ArduinoJson/
#
#        ROOT CAUSE (C3-GATE-002-R1 corrected via dependency matrix):
#          ArduinoJson v6.19.0 has a specific bug: VariantSlot::setKey()
#          dereferences a null pointer when addMember() returns null due
#          to an exhausted JsonDocument memory pool. This causes SIGSEGV
#          when publishStatus() / _publishPirAck() populate a
#          DynamicJsonDocument(6144) with full device state (12 channels
#          × 4 schedules + PIRs + telemetry) and the pool overflows.
#
#          ArduinoJson v6.19.1 explicitly fixes this bug per upstream
#          release notes: "Fix crash when adding an object member in a
#          too small JsonDocument". All subsequent versions (6.19.2+,
#          6.20.x, 6.21.x) inherit the fix.
#
#          Dependency matrix evidence (tested with identical host shim):
#            v6.18.2  → PASS (predates bug introduction)
#            v6.19.0  → CRASH (SIGSEGV at VariantSlot::setKey)
#            v6.19.1  → PASS (upstream fix — production-compatible)
#            v6.19.4  → PASS
#            v6.20.1  → PASS
#            v6.21.6  → PASS
#
#          PRODUCTION COMPATIBILITY:
#          Production firmware pins `bblanchon/ArduinoJson@^6.19.0` in
#          platformio.ini. Caret semver (^6.19.0) allows any compatible
#          version >= 6.19.0, < 7.0.0. PlatformIO resolves this to the
#          latest 6.x on ESP32 (where the real Arduino String class is
#          fully compatible with v6.19.1+).
#
#          Host-test environment uses v6.19.1 (the minimum production-
#          compatible version that fixes the crash). This is NOT a
#          divergence from production — v6.19.1 satisfies `^6.19.0` and
#          is the exact version auditor's R1 finding identified as the
#          upstream fix.
#
#          PROVENANCE: this is a pre-existing host-test environment issue.
#          The crash trigger (shim _connected=true default) was introduced
#          in commit 677d386 (F-P0-1 correction 4 — pre-C3). C3 commit
#          4cd5d0b did NOT modify the PubSubClient section of
#          MqttClientDeps.h nor MqttClient.cpp — see C3-GATE-002
#          auditor disposition for full provenance evidence.
#
#     2. arduinojson_compat.h must be available on the include path.
#        - ArduinoJson v6.x references `::StringSumHelper` (a class returned
#          by Arduino's String::operator+) via the IsString trait when
#          ARDUINOJSON_ENABLE_ARDUINO_STRING=1 (set by MqttClientDeps.h:186).
#          The host shim's String class does not provide StringSumHelper.
#          Forward-declaring it as an empty class is sufficient — the trait
#          is never instantiated because production code never returns
#          StringSumHelper (it uses operator+= and explicit String ctors).
#        - This file is auto-created by this script at
#          firmware/test/host/shims/arduinojson_compat.h
#
# USAGE:
#   cd firmware/test/host
#   ./setup_host_env.sh
#
#   Then any `make -f Makefile.* run` will work.
#
# IDEMPOTENT: Yes. Re-running skips already-satisfied steps.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LIBDEPS_DIR="${FIRMWARE_DIR}/.pio/libdeps/development"
ARDUINOJSON_DIR="${LIBDEPS_DIR}/ArduinoJson"
# v6.19.1 is the upstream fix for "crash when adding an object member in a
# too small JsonDocument". Production platformio.ini pins ^6.19.0 (caret
# allows v6.19.1). Using v6.19.1 here keeps host test environment aligned
# with production dependency declaration — no divergence.
ARDUINOJSON_VERSION="v6.19.1"
COMPAT_HEADER="${SCRIPT_DIR}/shims/arduinojson_compat.h"

echo "=== Host Test Environment Setup ==="
echo "Script dir:  ${SCRIPT_DIR}"
echo "Firmware dir: ${FIRMWARE_DIR}"
echo ""

# ----------------------------------------------------------------------------
# Step 1: Install ArduinoJson v6.19.1 (production-compatible, fixes v6.19.0 crash)
# ----------------------------------------------------------------------------
# Accept any v6.19.1+ as valid (caret semver like production). For simplicity
# we pin to exact v6.19.1 — the minimum version that fixes the crash and
# satisfies production `^6.19.0` constraint.
ACCEPTED_VERSIONS="6.19.1 6.19.2 6.19.3 6.19.4 6.20.0 6.20.1 6.21.0 6.21.1 6.21.2 6.21.3 6.21.4 6.21.5 6.21.6"

if [ -f "${ARDUINOJSON_DIR}/src/ArduinoJson.h" ]; then
  INSTALLED_VERSION=$(grep -E "^version=" "${ARDUINOJSON_DIR}/library.properties" 2>/dev/null | cut -d= -f2 || echo "unknown")
  if echo "${ACCEPTED_VERSIONS}" | grep -qw "${INSTALLED_VERSION}"; then
    echo "[OK] ArduinoJson v${INSTALLED_VERSION} already installed at ${ARDUINOJSON_DIR}"
    echo "     (accepted: v6.19.1+ — production-compatible with ^6.19.0 pin)"
  else
    echo "[WARN] ArduinoJson found but version is ${INSTALLED_VERSION}"
    echo "       v6.19.0 has a known crash bug (fixed in v6.19.1 per upstream release notes)."
    echo "       v6.18.x works but predates the production minimum (^6.19.0)."
    echo "       Replacing with ${ARDUINOJSON_VERSION} (production-compatible fix)..."
    rm -rf "${ARDUINOJSON_DIR}"
    git clone --depth 1 --branch "${ARDUINOJSON_VERSION}" \
      https://github.com/bblanchon/ArduinoJson.git "${ARDUINOJSON_DIR}" 2>&1 | tail -2
    echo "[OK] ArduinoJson ${ARDUINOJSON_VERSION} installed"
  fi
else
  echo "[INSTALL] Cloning ArduinoJson ${ARDUINOJSON_VERSION} to ${ARDUINOJSON_DIR}..."
  mkdir -p "${LIBDEPS_DIR}"
  git clone --depth 1 --branch "${ARDUINOJSON_VERSION}" \
    https://github.com/bblanchon/ArduinoJson.git "${ARDUINOJSON_DIR}" 2>&1 | tail -2
  echo "[OK] ArduinoJson ${ARDUINOJSON_VERSION} installed"
fi
echo ""

# ----------------------------------------------------------------------------
# Step 2: Ensure arduinojson_compat.h test-only header exists
# ----------------------------------------------------------------------------
if [ -f "${COMPAT_HEADER}" ]; then
  echo "[OK] arduinojson_compat.h already exists at ${COMPAT_HEADER}"
else
  echo "[CREATE] Writing ${COMPAT_HEADER}..."
  cat > "${COMPAT_HEADER}" <<'EOF'
// =============================================================================
// arduinojson_compat.h — Test-only ArduinoJson compatibility shim
// =============================================================================
// P2-2 F-P0-2 C3-GATE-002 TEST-INFRASTRUCTURE CORRECTION
//
// Forward-declares ::StringSumHelper so ArduinoJson v6.x's ARDUINOJSON_ENABLE_
// ARDUINO_STRING adapter can specialize IsString<StringSumHelper> without
// requiring the full Arduino String implementation.
//
// This is a TEST-ONLY build artifact (committed under firmware/test/host/
// shims/). Used via `g++ -include shims/arduinojson_compat.h` in the host
// test Makefiles to enable host builds against production source.
//
// Background: ArduinoJson v6.x's ArduinoStringAdapter.hpp references
// ::StringSumHelper via a template trait specialization. The host-test shim
// in firmware/test/host/shims/Arduino.h provides a working ::String class
// but not ::StringSumHelper. Forward-declaring it as an empty class is
// sufficient — the trait is never instantiated because production firmware
// never returns StringSumHelper (it uses operator+= and explicit String
// constructors exclusively).
//
// This is a TEST-INFRASTRUCTURE correction, NOT a C3 semantic fix. It does
// NOT change production behavior, canonical hash schema, MQTT semantics,
// RestJournalHelper contract, or F11 behavior. See HOST_ENV_README.md for
// full C3-GATE-002 disposition.
// =============================================================================
#pragma once
class StringSumHelper;
EOF
  echo "[OK] arduinojson_compat.h created"
fi
echo ""

# ----------------------------------------------------------------------------
# Step 3: Verify setup
# ----------------------------------------------------------------------------
echo "=== Verification ==="
INSTALLED_VERSION=$(grep -E "^version=" "${ARDUINOJSON_DIR}/library.properties" 2>/dev/null | cut -d= -f2 || echo "unknown")
if [ -f "${ARDUINOJSON_DIR}/src/ArduinoJson.h" ] && \
   echo "${ACCEPTED_VERSIONS}" | grep -qw "${INSTALLED_VERSION}" && \
   [ -f "${COMPAT_HEADER}" ]; then
  echo "[PASS] Host test environment is ready"
  echo "       ArduinoJson version: ${INSTALLED_VERSION} (accepted: v6.19.1+)"
  echo "       Compat header: ${COMPAT_HEADER}"
  echo ""
  echo "You can now run:"
  echo "  cd firmware/test/host"
  echo "  make -f Makefile.tj run    # TransactionJournalTest (194/194)"
  echo "  make -f Makefile.cr run    # CommandRoutingTest (133/133)"
  echo "  make -f Makefile.che run   # CommandHashEquivalenceTest (26/26)"
  echo "  make -f Makefile.ws run    # WebServerTest (111/111)"
  echo "  make -f Makefile.mc run    # MqttClientTest (31/31)"
  echo "  make -f Makefile.chb run   # CommandHashBaseline (14 vectors)"
  echo ""
  echo "Or run all: ./run_all_host_tests.sh"
  exit 0
else
  echo "[FAIL] Setup incomplete — check errors above"
  echo "       Installed version: ${INSTALLED_VERSION}"
  echo "       Accepted versions: ${ACCEPTED_VERSIONS}"
  exit 1
fi
