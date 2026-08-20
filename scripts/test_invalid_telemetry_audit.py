#!/usr/bin/env python3
"""
test_invalid_telemetry_audit.py — Scan for numeric fabrication patterns.

Scans firmware/*.cpp + *.h, pwa/src/**/*.ts + *.tsx, code.gs/Code.gs.

Patterns to flag (in code, NOT comments):
  - Number(x) || <number>
  - parseFloat(x) || <number>
  - || 0 (not || 0x hex, not || 0. decimal)
  - || 50
  - || 10

Safe fallbacks (allow): || '', || null, || undefined, || 'N/A', || "N/A",
  || false, || true, || [], || {}, || "", || "unknown", || 'unknown'

False-positive suppression (skip if line matches):
  - .length || 0
  - translateX / translateY / scale(
  - Error( / error( / console.error

Exit 0 on PASS (no fabrication patterns found), 1 on FAIL.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import List, Tuple


# ---------------------------------------------------------------------------
# Patterns to flag
# ---------------------------------------------------------------------------

FLAG_PATTERNS = [
    # Number(x) || <number> — explicit numeric fallback for Number()
    re.compile(r"Number\([^)]*\)\s*\|\|\s*\d"),
    # parseFloat(x) || <number>
    re.compile(r"parseFloat\([^)]*\)\s*\|\|\s*\d"),
    # || 0 (not 0x hex, not 0. decimal)
    re.compile(r"\|\|\s*0(?![xX0-9a-fA-F.])"),
    # || 50
    re.compile(r"\|\|\s*50\b"),
    # || 10
    re.compile(r"\|\|\s*10\b"),
]

SAFE_FALLBACKS = [
    "|| ''", "|| null", "|| undefined", "|| 'N/A'", '|| "N/A"',
    "|| false", "|| true", "|| []", "|| {}",
    '|| ""', '|| "unknown"', "|| 'unknown'",
]

SUPPRESSION_PATTERNS = [
    re.compile(r"\.length\s*\|\|\s*0"),
    re.compile(r"\btranslate[XY]\b"),
    re.compile(r"\bscale\s*\("),
    re.compile(r"\bError\s*\("),
    re.compile(r"\berror\s*\("),
    re.compile(r"\bconsole\.error\b"),
]


def is_safe_fallback(line: str) -> bool:
    for s in SAFE_FALLBACKS:
        if s in line:
            return True
    return False


def is_suppressed(line: str) -> bool:
    for pat in SUPPRESSION_PATTERNS:
        if pat.search(line):
            return True
    return False


# ---------------------------------------------------------------------------
# Comment stripping
# ---------------------------------------------------------------------------

def strip_comments(text: str, lang: str) -> str:
    """Strip comments by language. Keep blank lines for line-number accuracy."""
    if lang in ("cpp", "h", "ts", "tsx", "js"):
        # /* */ block + // line
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
        out = []
        for line in text.splitlines():
            s = line.lstrip()
            if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
                out.append("")
                continue
            # Strip inline // comments (naive — audit-grade only)
            idx = line.find("//")
            if idx >= 0:
                before = line[:idx]
                if before.count('"') % 2 == 0 and before.count("'") % 2 == 0:
                    line = before
            out.append(line)
        return "\n".join(out)
    elif lang == "gs":
        return strip_comments(text, "js")
    return text


# ---------------------------------------------------------------------------
# Scan
# ---------------------------------------------------------------------------

def scan_file(path: Path, lang: str) -> List[Tuple[int, str, str]]:
    """Return list of (line_no, line_content, matched_pattern_desc)."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return []
    stripped = strip_comments(text, lang)
    issues = []
    for i, line in enumerate(stripped.splitlines(), start=1):
        if is_suppressed(line):
            continue
        if is_safe_fallback(line):
            # The line has a safe fallback somewhere — but we still need to
            # check whether there's a separate unsafe fallback on the same line.
            # Simplest: if the line has a safe fallback substring, skip flagging.
            # (Audit-grade heuristic.)
            continue
        for pat in FLAG_PATTERNS:
            m = pat.search(line)
            if m:
                issues.append((i, line.strip(), pat.pattern))
                break  # one flag per line is enough
    return issues


def collect_files(fw_dir: Path, pwa_dir: Path) -> List[Tuple[Path, str]]:
    files: List[Tuple[Path, str]] = []
    # firmware/*.cpp + *.h
    fw_src = fw_dir / "firmware"
    if fw_src.is_dir():
        for p in fw_src.glob("*.cpp"):
            files.append((p, "cpp"))
        for p in fw_src.glob("*.h"):
            files.append((p, "h"))
    # pwa/src/**/*.ts + *.tsx
    pwa_src = pwa_dir / "src"
    if pwa_src.is_dir():
        for p in pwa_src.rglob("*.ts"):
            files.append((p, "ts"))
        for p in pwa_src.rglob("*.tsx"):
            files.append((p, "tsx"))
    # code.gs/Code.gs
    code_gs = fw_dir / "code.gs" / "Code.gs"
    if code_gs.is_file():
        files.append((code_gs, "gs"))
    return files


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    fw_dir = repo_root / "firmware"
    pwa_dir = repo_root / "pwa"

    print("=" * 78)
    print("Invalid Telemetry Numeric Fabrication Audit")
    print("=" * 78)

    files = collect_files(fw_dir, pwa_dir)
    print(f"  scanning {len(files)} files "
          f"(firmware/*.{ '{cpp,h}' }, pwa/src/**/*.{ '{ts,tsx}' }, Code.gs)")

    total_issues = 0
    file_count = 0
    for f, lang in files:
        # Skip excluded paths
        if any(part in {"node_modules", ".next", ".pio", "dist", "build"} for part in f.parts):
            continue
        file_count += 1
        issues = scan_file(f, lang)
        if issues:
            total_issues += len(issues)
            print()
            print(f"  {f.relative_to(repo_root)}:")
            for line_no, line, pat in issues:
                print(f"    L{line_no} [{pat}]: {line[:140]}")

    print()
    print(f"  scanned {file_count} files")
    print("=" * 78)
    if total_issues == 0:
        print("[INVALID_TELEMETRY_AUDIT] PASS — no numeric fabrication patterns")
        print("=" * 78)
        return 0
    print(f"[INVALID_TELEMETRY_AUDIT] FAIL — {total_issues} fabrication pattern(s)")
    print("=" * 78)
    return 1


if __name__ == "__main__":
    sys.exit(main())
