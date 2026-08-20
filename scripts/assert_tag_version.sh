#!/usr/bin/env bash
# =============================================================================
# Pre-Tag Version Assertion — prevents REAUDIT-FW-VER-001 recurrence
# =============================================================================
# This script MUST be run BEFORE creating any git tag. It verifies that
# Config.h::FIRMWARE_VERSION matches the intended tag.
#
# Usage: ./scripts/assert_tag_version.sh v4.3.10
#
# Exit 0 = OK to tag. Exit 1 = MISMATCH — fix Config.h first.
# =============================================================================
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 <tag-name> (e.g., v4.3.10)"
  exit 1
fi

TAG="$1"
TAG_NUM="${TAG#v}"  # strip 'v' prefix

# Extract FIRMWARE_VERSION from Config.h
VERSION=$(grep 'FIRMWARE_VERSION\[\]' firmware/Config.h | sed 's/.*"\([^"]*\)".*/\1/')

# Extract BUILD_DATE from Config.h
BUILD_DATE=$(grep 'BUILD_DATE\[\]' firmware/Config.h | head -1 | sed 's/.*"\([^"]*\)".*/\1/')

echo "Tag:           $TAG (numeric: $TAG_NUM)"
echo "FIRMWARE_VERSION: $VERSION"
echo "BUILD_DATE:       $BUILD_DATE"

# Check FIRMWARE_VERSION matches tag
if [ "$VERSION" != "$TAG_NUM" ]; then
  echo ""
  echo "❌ MISMATCH: Config.h FIRMWARE_VERSION='$VERSION' but tag='$TAG'"
  echo "   Fix: sed -i 's/FIRMWARE_VERSION\\[\\] = \"$VERSION\"/FIRMWARE_VERSION[] = \"$TAG_NUM\"/' firmware/Config.h"
  echo "   Then re-run this script."
  exit 1
fi

# Check BUILD_DATE contains the version (not a stale version from prior release)
if ! echo "$BUILD_DATE" | grep -q "$TAG_NUM"; then
  echo ""
  echo "❌ MISMATCH: BUILD_DATE='$BUILD_DATE' does not contain '$TAG_NUM'"
  echo "   Fix: sed -i 's|BUILD_DATE\\[\\] = \"$BUILD_DATE\"|BUILD_DATE[] = \"v${TAG_NUM}-release\"|' firmware/Config.h"
  echo "   Then re-run this script."
  exit 1
fi

echo ""
echo "✅ PASS: FIRMWARE_VERSION=$VERSION matches tag=$TAG"
echo "✅ PASS: BUILD_DATE=$BUILD_DATE contains $TAG_NUM"
echo "   Safe to create tag."
