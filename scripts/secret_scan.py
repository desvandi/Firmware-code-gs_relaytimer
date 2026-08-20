#!/usr/bin/env python3
"""
secret_scan.py — Pre-commit secret scanner.

Scans both repos for leaked credentials:
  - GitHub PATs (ghp_, github_pat_, gho_, ghu_, ghs_)
  - Google API keys (AIza…)
  - AWS access keys (AKIA…)
  - PEM private keys
  - Hardcoded JWT secrets
  - Hardcoded MQTT passwords
  - Hardcoded GAS HMAC secrets
  - Generic passwords (WARN)
  - Connection strings (mongodb://, postgres://, etc.)
  - Bearer tokens (WARN)

CRITICAL findings → exit 1 (blocks commit).
WARN findings → exit 0 (does not block).

Exit 0 on PASS (no CRITICAL), 0 on WARN, 1 on FAIL (CRITICAL).
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import List, Tuple


# ---------------------------------------------------------------------------
# Patterns
# ---------------------------------------------------------------------------

CRITICAL_PATTERNS = [
    ("GitHub PAT (ghp_)", r"ghp_[A-Za-z0-9]{36,}"),
    ("GitHub PAT (github_pat_)", r"github_pat_[A-Za-z0-9_]{22,}"),
    ("GitHub PAT (gho_)", r"gho_[A-Za-z0-9]{36,}"),
    ("GitHub PAT (ghu_)", r"ghu_[A-Za-z0-9]{36,}"),
    ("GitHub PAT (ghs_)", r"ghs_[A-Za-z0-9]{36,}"),
    ("Google API key", r"AIza[0-9A-Za-z_\-]{35}"),
    ("AWS access key", r"AKIA[0-9A-Z]{16}"),
    ("PEM private key",
     r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP )?PRIVATE KEY-----"),
    ("Hardcoded JWT secret",
     r"(?:JWT_SECRET|jwt_secret|jwtSecret|signing_key|SIGNING_KEY)\s*[:=]\s*[\"'][A-Za-z0-9+/=]{32,}[\"']"),
    ("Hardcoded MQTT password",
     r"(?:MQTT_PASSWORD|mqtt_password|mqttPass)\s*[:=]\s*[\"'][^\"']{8,}[\"']"),
    ("Hardcoded GAS HMAC secret",
     r"(?:GAS_SECRET|gas_secret|gasSecret|DEVICE_SECRET)\s*[:=]\s*[\"'][0-9a-fA-F]{64}[\"']"),
    ("Connection string",
     r"(?:mongodb|postgres|mysql|redis|amqp)://[^:\s]+:[^@\s]+@"),
]

WARN_PATTERNS = [
    ("Generic hardcoded password",
     r"(?:password|passwd|pwd)\s*[:=]\s*[\"'][A-Za-z0-9!@#$%^&*()_+\-=]{8,}[\"']"),
    ("Bearer token",
     r"(?:Bearer|bearer)\s+[A-Za-z0-9_\-\.]{40,}"),
]

# Skip directories
SKIP_DIRS = {
    ".git", "node_modules", ".next", ".pio", "__pycache__", ".venv",
    "venv", ".cache", "dist", "build", ".gradle", ".idea", ".vscode",
}

# Skip files
SKIP_FILES = {
    ".env.example",
    "assert_version_contract.py",
    "test_ed25519_rfc8032_kat.py",
    # This file itself contains regex patterns that would self-match
    "secret_scan.py",
}

# Skip extensions
SKIP_EXT = {
    ".bin", ".elf", ".map", ".log", ".lock",
    ".jpg", ".jpeg", ".png", ".gif", ".svg", ".ico",
    ".pdf", ".zip", ".tar", ".gz", ".tgz",
    ".mp4", ".mp3", ".wav",
}

MAX_FILE_SIZE = 1 * 1024 * 1024  # 1 MB

# Safe line patterns (skip)
SAFE_LINE_PATTERNS = [
    re.compile(r"^\s*//"),
    re.compile(r"^\s*\*"),
    re.compile(r"^\s*#"),
    re.compile(r"^\s*/\*"),
    re.compile(r"process\.env\."),
    re.compile(r"getenv\("),
    re.compile(r"PropertiesService\.getScriptProperties\(\)\.getProperty"),
    re.compile(r"prefs\.getString"),
    re.compile(r"NVS_KEY_"),
    re.compile(r"constexpr.*KEY.*="),
    re.compile(r'"Timer12-v4\.0-CHANGE-ME-IN-PRODUCTION"'),
    re.compile(r"admin123"),
    re.compile(r"timer12-dev-only-secret"),
    re.compile(r"placeholder", re.IGNORECASE),
    re.compile(r"example", re.IGNORECASE),
    re.compile(r"YOUR_"),
    re.compile(r"REPLACE_"),
    re.compile(r"CHANGE_ME"),
    re.compile(r"<your", re.IGNORECASE),
    re.compile(r"\$\{"),
]


# ---------------------------------------------------------------------------
# Scanning
# ---------------------------------------------------------------------------

def is_safe_line(line: str) -> bool:
    for pat in SAFE_LINE_PATTERNS:
        if pat.search(line):
            return True
    return False


def scan_file(path: Path) -> Tuple[List[Tuple[str, str, int, str]],
                                  List[Tuple[str, str, int, str]]]:
    """Return (critical_findings, warn_findings). Each finding is a tuple of
    (rule_name, file_path, line_no, line_content)."""
    critical: List[Tuple[str, str, int, str]] = []
    warns: List[Tuple[str, str, int, str]] = []

    try:
        size = path.stat().st_size
    except Exception:
        return (critical, warns)
    if size > MAX_FILE_SIZE:
        return (critical, warns)
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return (critical, warns)

    for i, line in enumerate(text.splitlines(), start=1):
        if is_safe_line(line):
            continue
        for rule_name, pat in CRITICAL_PATTERNS:
            if re.search(pat, line):
                critical.append((rule_name, str(path), i, line.strip()[:200]))
        for rule_name, pat in WARN_PATTERNS:
            if re.search(pat, line):
                warns.append((rule_name, str(path), i, line.strip()[:200]))
    return (critical, warns)


def collect_files(repo_root: Path) -> List[Path]:
    files: List[Path] = []
    for p in repo_root.rglob("*"):
        if not p.is_file():
            continue
        if any(part in SKIP_DIRS for part in p.parts):
            continue
        if p.name in SKIP_FILES:
            continue
        if p.suffix.lower() in SKIP_EXT:
            continue
        files.append(p)
    return files


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".",
                        help="Repo root (parent of firmware/ + pwa/)")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()

    print("=" * 78)
    print("Secret Scanner — Pre-Commit Credential Audit")
    print("=" * 78)
    print(f"  repo root: {root}")

    all_critical: List[Tuple[str, str, int, str]] = []
    all_warns: List[Tuple[str, str, int, str]] = []

    # Scan both repos
    for repo_dir in [root / "firmware", root / "pwa", root]:
        if not repo_dir.is_dir():
            continue
        print(f"  scanning: {repo_dir.relative_to(root) if repo_dir != root else repo_dir}")
        files = collect_files(repo_dir)
        for f in files:
            # Skip the secret_scan.py itself (already excluded, but double-check)
            if f.name == "secret_scan.py":
                continue
            crit, warns = scan_file(f)
            all_critical.extend(crit)
            all_warns.extend(warns)

    print()
    print(f"  CRITICAL findings: {len(all_critical)}")
    print(f"  WARN findings:     {len(all_warns)}")

    if all_critical:
        print()
        print("CRITICAL — blocking commit:")
        for rule, fpath, lineno, line in all_critical[:40]:
            try:
                rel = Path(fpath).relative_to(root)
            except ValueError:
                rel = Path(fpath)
            print(f"  [{rule}] {rel}:{lineno}")
            print(f"    {line}")
        if len(all_critical) > 40:
            print(f"  ... and {len(all_critical) - 40} more")

    if all_warns:
        print()
        print("WARN (informational — does NOT block):")
        for rule, fpath, lineno, line in all_warns[:10]:
            try:
                rel = Path(fpath).relative_to(root)
            except ValueError:
                rel = Path(fpath)
            print(f"  [{rule}] {rel}:{lineno}")
            print(f"    {line}")
        if len(all_warns) > 10:
            print(f"  ... and {len(all_warns) - 10} more")

    print()
    print("=" * 78)
    if not all_critical:
        if all_warns:
            print("[SECRET_SCAN] PASS (with WARN — non-blocking warnings)")
        else:
            print("[SECRET_SCAN] PASS — no secrets found")
        print("=" * 78)
        return 0
    print("[SECRET_SCAN] FAIL — CRITICAL secrets detected")
    print("=" * 78)
    return 1


if __name__ == "__main__":
    sys.exit(main())
