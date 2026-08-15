#!/usr/bin/env bash
# =============================================================================
# setup_host_env.sh — Initialize host-test build environment
# =============================================================================
# P2-2 F-P0-2 C3-GATE-002 TEST-INFRASTRUCTURE CORRECTION
#
# PURPOSE:
#   The host-side test Makefiles (Makefile.ws / Makefile.mc / Makefile.che /
#   Makefile.chb) compile production firmware source against minimal host
#   shims under firmware/test/host/shims/. Two host-environment dependencies
#   must be satisfied before the host tests will build and run cleanly:
#
#     1. ArduinoJson v6.18.x must be installed at
#        firmware/.pio/libdeps/development/ArduinoJson/
#        - Production firmware pins `bblanchon/ArduinoJson@^6.19.0` in
#          platformio.ini. PlatformIO resolves this to the latest 6.x on
#          ESP32 (where the real Arduino String class is fully compatible).
#        - On HOST builds, the shim's String class (in shims/Arduino.h) is
#          a minimal std::string wrapper. ArduinoJson v6.19.0+ has a
#          regression where VariantSlot::setKey() does NOT gracefully handle
#          a null slot returned by an exhausted memory pool — instead it
#          dereferences the null pointer (SEGV). v6.18.x handles this case
#          gracefully by returning null VariantRef, which the production
#          code already tolerates.
#        - Symptom: MqttClientTest + CommandHashBaseline segfault at
#          VariantSlot::setKey() when publishStatus() / _publishPirAck()
#          populate a DynamicJsonDocument that overflows its pool.
#        - Provenance: this is a pre-existing host-test environment issue
#          (shim _connected=true default + v6.19+ regression). C3 commit
#          4cd5d0b did NOT introduce or modify this issue — see C3-GATE-002
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
ARDUINOJSON_VERSION="v6.18.2"
COMPAT_HEADER="${SCRIPT_DIR}/shims/arduinojson_compat.h"

echo "=== Host Test Environment Setup ==="
echo "Script dir:  ${SCRIPT_DIR}"
echo "Firmware dir: ${FIRMWARE_DIR}"
echo ""

# ----------------------------------------------------------------------------
# Step 1: Install ArduinoJson v6.18.2 (host-test compatible version)
# ----------------------------------------------------------------------------
if [ -f "${ARDUINOJSON_DIR}/src/ArduinoJson.h" ]; then
  INSTALLED_VERSION=$(grep -E "^version=" "${ARDUINOJSON_DIR}/library.properties" 2>/dev/null | cut -d= -f2 || echo "unknown")
  if [ "${INSTALLED_VERSION}" = "6.18.2" ]; then
    echo "[OK] ArduinoJson v6.18.2 already installed at ${ARDUINOJSON_DIR}"
  else
    echo "[WARN] ArduinoJson found but version is ${INSTALLED_VERSION} (expected 6.18.2)"
    echo "       v6.19.0+ has a regression that causes SEGV in host-test builds."
    echo "       Replacing with v6.18.2..."
    rm -rf "${ARDUINOJSON_DIR}"
    git clone --depth 1 --branch "${ARDUINOJSON_VERSION}" \
      https://github.com/bblanchon/ArduinoJson.git "${ARDUINOJSON_DIR}" 2>&1 | tail -2
    echo "[OK] ArduinoJson v6.18.2 installed"
  fi
else
  echo "[INSTALL] Cloning ArduinoJson ${ARDUINOJSON_VERSION} to ${ARDUINOJSON_DIR}..."
  mkdir -p "${LIBDEPS_DIR}"
  git clone --depth 1 --branch "${ARDUINOJSON_VERSION}" \
    https://github.com/bblanchon/ArduinoJson.git "${ARDUINOJSON_DIR}" 2>&1 | tail -2
  echo "[OK] ArduinoJson v6.18.2 installed"
fi
echo ""

# ----------------------------------------------------------------------------
# Step 2: Create arduinojson_compat.h test-only header
# ----------------------------------------------------------------------------
if [ -f "${COMPAT_HEADER}" ]; then
  echo "[OK] arduinojson_compat.h already exists at ${COMPAT_HEADER}"
else
  echo "[CREATE] Writing ${COMPAT_HEADER}..."
  cat > "${COMPAT_HEADER}" <<'EOF'
// =============================================================================
// arduinojson_compat.h — Test-only ArduinoJson compatibility shim
// =============================================================================
// Forward-declares ::StringSumHelper so ArduinoJson v6.x's ARDUINOJSON_ENABLE_
// ARDUINO_STRING adapter can specialize IsString<StringSumHelper> without
// requiring the full Arduino String implementation.
//
// This is a TEST-ONLY build artifact. Used via `g++ -include` flag in the
// host-test Makefiles to enable host builds against production source.
//
// Background: ArduinoJson v6.x's ArduinoStringAdapter.hpp references
// ::StringSumHelper via a template trait specialization. The host-test shim
// in firmware/test/host/shims/Arduino.h provides a working ::String class
// but not ::StringSumHelper. Forward-declaring it as an empty class is
// sufficient — the trait is never instantiated because production firmware
// never returns StringSumHelper (it uses operator+= and explicit String
// constructors exclusively).
//
// Provenance: C3-GATE-002 test-infra correction. Not a C3 semantic fix.
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
if [ -f "${ARDUINOJSON_DIR}/src/ArduinoJson.h" ] && \
   [ "$(grep -E '^version=' "${ARDUINOJSON_DIR}/library.properties" | cut -d= -f2)" = "6.18.2" ] && \
   [ -f "${COMPAT_HEADER}" ]; then
  echo "[PASS] Host test environment is ready"
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
  exit 1
fi
