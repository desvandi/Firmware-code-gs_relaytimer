#!/usr/bin/env python3
"""
assert_version_contract.py — Validate firmware = PWA = documentation version.

Asserts:
1. firmware/firmware/Config.h :: FIRMWARE_VERSION matches
2. pwa/package.json :: version matches
3. All .md files in firmware/ + pwa/ have a title line that EITHER:
   - references the expected version (e.g., "v4.3.8" or "4.3.8"), OR
   - is version-agnostic (no version string at all in the title)
4. Body-scan (two-tier) — only flags bare version mentions of stale versions
   (v4.2.0, v4.1.0, v4.0.0) in current-state sentences. Compat-table rows,
   version ranges, and historical/audit lines are exempt.
5. Git tag at HEAD (if HEAD is tagged): tag must match expected version after
   stripping the leading 'v' prefix.

Usage:
  python3 assert_version_contract.py [--root <repo-root>]

Exit code:
  0 = PASS
  1 = FAIL
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import List, Tuple, Optional


# ---------------------------------------------------------------------------
# Path discovery
# ---------------------------------------------------------------------------

def find_firmware_config_h(root: Path) -> Optional[Path]:
    candidates = [
        root / "firmware" / "firmware" / "Config.h",
        root / "firmware" / "Config.h",
        root / "Config.h",
    ]
    for p in candidates:
        if p.is_file():
            return p
    return None


def find_pwa_package_json(root: Path) -> Optional[Path]:
    candidates = [
        root / "pwa" / "package.json",
        root / "package.json",
    ]
    for p in candidates:
        if p.is_file():
            return p
    return None


# ---------------------------------------------------------------------------
# Version extraction
# ---------------------------------------------------------------------------

FW_VER_RE = re.compile(r'FIRMWARE_VERSION\[\]\s*=\s*"([^"]+)"')


def extract_firmware_version(config_h: Path) -> Optional[str]:
    text = config_h.read_text(encoding="utf-8", errors="replace")
    m = FW_VER_RE.search(text)
    return m.group(1) if m else None


def extract_pwa_version(package_json: Path) -> Optional[str]:
    try:
        data = json.loads(package_json.read_text(encoding="utf-8"))
    except Exception:
        return None
    return data.get("version")


# ---------------------------------------------------------------------------
# Git tag at HEAD
# ---------------------------------------------------------------------------

def get_git_tag_at_head(repo_dir: Path) -> Optional[str]:
    if not (repo_dir / ".git").is_dir():
        # Not a git checkout — skip tag check
        return None
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_dir), "tag", "--points-at", "HEAD"],
            capture_output=True, text=True, timeout=10,
        )
        tags = [t.strip() for t in out.stdout.splitlines() if t.strip()]
        return tags[0] if tags else None
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Title-line version scan
# ---------------------------------------------------------------------------

TITLE_VERSION_RE = re.compile(r'\bv?(\d+\.\d+\.\d+)\b')
STALE_VERSIONS = {"4.2.0", "4.1.0", "4.0.0"}
STALE_VERSION_PATTERNS = [r'v4\.2\.0', r'v4\.1\.0', r'v4\.0\.0']

# Tier-2 body scan exemption patterns
BODY_EXEMPTION_SUBSTRINGS = [
    "backward", "forward", "legacy", "compat", "history",
    "released in", "introduced in", "stale", "reconciled",
    "audit", "previously",
    "v3 (firmware", "v4 (firmware", "v5 (firmware",
    "(firmware v4.",
    "v4.x", "v4.3+", "v4.2.x", "v4.1.x", "v4.0.x",
    "4.3+", "through v4.", "from v4.",
]


def is_version_agnostic_title(title: str) -> bool:
    return not TITLE_VERSION_RE.search(title)


def title_references_version(title: str, expected: str) -> bool:
    # Look for either "4.3.8" or "v4.3.8" in the title line
    return (expected in title) or (f"v{expected}" in title)


def is_compat_table_row(line: str) -> bool:
    # Lines with | are markdown table rows — exempt from stale-version flag
    return "|" in line


def is_version_range(line: str) -> bool:
    # E.g., "v4.2.x", "v4.3+", "through v4.x", "from v4.0 to v4.3"
    if re.search(r'v?\d+\.\d+\.(x|\+)', line):
        return True
    if re.search(r'(through|from)\s+v?\d+\.\d+', line, re.IGNORECASE):
        return True
    if re.search(r'v?\d+\.\d+\.\d+\s*[-–—]\s*v?\d+', line):
        return True
    return False


def body_line_is_exempt(line: str) -> bool:
    if is_compat_table_row(line):
        return True
    if is_version_range(line):
        return True
    lower = line.lower()
    for sub in BODY_EXEMPTION_SUBSTRINGS:
        if sub.lower() in lower:
            return True
    return False


def scan_markdown_file(md_path: Path, expected_version: str) -> List[str]:
    """Return list of issues found in this file (empty = OK)."""
    issues: List[str] = []
    try:
        text = md_path.read_text(encoding="utf-8", errors="replace")
    except Exception as e:
        return [f"{md_path}: cannot read ({e})"]

    lines = text.splitlines()
    if not lines:
        return []

    # ---- Tier 1: title-line strict scan ----
    title = lines[0].strip()
    title_lower = title.lower()
    # Skip pure version-agnostic titles (no version string anywhere)
    if not is_version_agnostic_title(title):
        # Title contains a version string. It MUST be the expected version.
        # Reject titles that reference a stale version
        for stale_ver in STALE_VERSIONS:
            if f"v{stale_ver}" in title or re.search(rf'\b{stale_ver}\b', title):
                # but allow if expected version is also present (e.g., upgrade notice)
                if expected_version in title or f"v{expected_version}" in title:
                    continue
                issues.append(
                    f"{md_path}: title references stale version v{stale_ver} "
                    f"(expected v{expected_version}): {title[:120]}"
                )
        # Title contains a version that isn't expected and isn't in stale list
        for m in TITLE_VERSION_RE.finditer(title):
            ver = m.group(1)
            if ver != expected_version and ver not in STALE_VERSIONS:
                issues.append(
                    f"{md_path}: title references unknown version {ver} "
                    f"(expected v{expected_version}): {title[:120]}"
                )

    # ---- Tier 2: body bare-version scan for stale versions only ----
    for i, line in enumerate(lines[1:], start=2):
        if body_line_is_exempt(line):
            continue
        for pat in STALE_VERSION_PATTERNS:
            if re.search(pat, line):
                issues.append(
                    f"{md_path}:{i}: stale version reference {pat} in body: "
                    f"{line.strip()[:120]}"
                )
                break

    return issues


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

SKIP_DIRS = {
    "node_modules", ".git", ".next", ".pio", "__pycache__", ".venv",
    "venv", ".cache", "dist", "build", ".gradle", ".idea", ".vscode",
}


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", default=".",
        help="Repo root (parent of firmware/ + pwa/)",
    )
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()

    print(f"[VERSION_CONTRACT] repo root: {root}")

    # 1. Extract firmware version
    fw_config = find_firmware_config_h(root)
    if not fw_config:
        print("FAIL: firmware Config.h not found")
        return 1
    fw_version = extract_firmware_version(fw_config)
    if not fw_version:
        print(f"FAIL: FIRMWARE_VERSION not found in {fw_config}")
        return 1
    print(f"  firmware version: {fw_version}  (from {fw_config.relative_to(root)})")

    # 2. Extract PWA version
    pwa_pkg = find_pwa_package_json(root)
    if not pwa_pkg:
        print("FAIL: pwa package.json not found")
        return 1
    pwa_version = extract_pwa_version(pwa_pkg)
    if not pwa_version:
        print(f"FAIL: version field not found in {pwa_pkg}")
        return 1
    print(f"  pwa version:      {pwa_version}  (from {pwa_pkg.relative_to(root)})")

    expected_version = fw_version
    if pwa_version != fw_version:
        print(f"FAIL: firmware ({fw_version}) != pwa ({pwa_version})")
        return 1

    print(f"  expected version (single source of truth): v{expected_version}")
    print()

    # 3. Scan .md files in firmware/ + pwa/ (excluding dependency dirs)
    md_dirs = [root / "firmware", root / "pwa"]
    all_issues: List[str] = []
    md_count = 0
    for md_dir in md_dirs:
        if not md_dir.is_dir():
            continue
        for md_path in md_dir.rglob("*.md"):
            # Skip dependency / build / VCS directories
            if any(part in SKIP_DIRS for part in md_path.parts):
                continue
            md_count += 1
            issues = scan_markdown_file(md_path, expected_version)
            all_issues.extend(issues)

    print(f"  scanned {md_count} .md files in firmware/ + pwa/")
    if all_issues:
        print(f"\nFAIL: {len(all_issues)} version-contract issue(s) found:")
        for iss in all_issues[:40]:
            print(f"  - {iss}")
        if len(all_issues) > 40:
            print(f"  ... and {len(all_issues) - 40} more")
        return 1
    print("  PASS: all markdown titles + bodies consistent")
    print()

    # 4. Git tag check (only if HEAD is tagged)
    for repo_dir in [root / "firmware", root / "pwa", root]:
        tag = get_git_tag_at_head(repo_dir)
        if tag is None:
            continue
        # Strip 'v' prefix
        tag_ver = tag[1:] if tag.startswith("v") else tag
        print(f"  git tag at HEAD in {repo_dir.relative_to(root) if repo_dir != root else repo_dir}: {tag}")
        if tag_ver != expected_version:
            print(f"FAIL: tag {tag} != expected version v{expected_version}")
            return 1
        print(f"  PASS: tag {tag} matches expected version")
        break
    else:
        print("  (no git tag at HEAD — skipping tag check)")

    print()
    print("=" * 60)
    print(f"[VERSION_CONTRACT] PASS — firmware = pwa = docs = v{expected_version}")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
