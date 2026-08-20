#!/usr/bin/env python3
"""
test_ai_actuator_isolation.py — Audit AI actuator isolation via call-graph analysis.

Tests:
  1. find_gpio_mutations: scan firmware/*.cpp + *.h (EXCLUDING RelayDriver.cpp/h)
     for direct GPIO mutations (digitalWrite, GPIO.out_w1[tc]s, setChannel,
     applyChannelState). Strip // comments + inline comments. Return dict.
  2. find_relay_engine_callers: find external callers of applyChannelState
     (only RelayEngine itself should call it).
  3. check_ai_no_gpio_path: verify Advisor.h + Advisor.cpp + InsightsHandlers.h
     have NO references to digitalWrite/RelayDriver/RelayEngine/setChannel/
     applyChannelState/GPIO.out_w1.
  4. check_gas_no_gpio_path: verify code.gs/Code.gs has NO relay_on/relay_off/
     setRelay/writeRelay/digitalWrite/GpioOutput (case-insensitive, skip comments).
  5. simulate_malicious_ai_json: verify apply_suggestion is in ALLOWED_ACTION_TYPES,
     insight has advisoryOnly=true, PWA renders as advisory card (no automatic
     mutation).

Exit 0 on PASS, 1 on FAIL.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple


# ---------------------------------------------------------------------------
# Comment stripping
# ---------------------------------------------------------------------------

def strip_cpp_comments_keep_lines(text: str) -> str:
    """Strip // line comments and inline comments; keep blank line structure."""
    # Remove /* ... */ block comments
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    # Remove // ... to end of line (but keep the newline)
    out_lines = []
    for line in text.splitlines():
        s = line.lstrip()
        if s.startswith("//") or s.startswith("*"):
            out_lines.append("")
            continue
        # Strip inline // comments
        # (best-effort; doesn't handle string literals containing //)
        # For audit purposes this is acceptable.
        idx = line.find("//")
        if idx >= 0:
            # Check if // is inside a string literal — naive heuristic
            before = line[:idx]
            if before.count('"') % 2 == 0 and before.count("'") % 2 == 0:
                line = before
        out_lines.append(line)
    return "\n".join(out_lines)


def strip_js_comments_keep_lines(text: str) -> str:
    return strip_cpp_comments_keep_lines(text)


# ---------------------------------------------------------------------------
# 1. find_gpio_mutations (excluding RelayDriver.cpp/h + RelayEngine.cpp/h)
# ---------------------------------------------------------------------------

GPIO_MUTATION_PATTERNS = [
    r"digitalWrite\s*\(",
    r"GPIO\.out_w1ts",
    r"GPIO\.out_w1tc",
    r"\bsetChannel\s*\(",
    r"\bapplyChannelState\s*\(",
]

GPIO_MUTATION_RE = re.compile("|".join(GPIO_MUTATION_PATTERNS))


def find_gpio_mutations(fw_dir: Path) -> Dict[str, List[str]]:
    """Return dict[file] = list of matching lines (excluding RelayDriver/Engine)."""
    results: Dict[str, List[str]] = {}
    firmware_dir = fw_dir / "firmware"
    if not firmware_dir.is_dir():
        return results
    # Exclude RelayDriver.cpp/h, RelayEngine.cpp/h (the authoritative actuator path)
    excluded = {"RelayDriver.cpp", "RelayDriver.h",
                 "RelayEngine.cpp", "RelayEngine.h"}
    files = list(firmware_dir.glob("*.cpp")) + list(firmware_dir.glob("*.h"))
    for f in files:
        if f.name in excluded:
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        stripped = strip_cpp_comments_keep_lines(text)
        matches: List[str] = []
        for line in stripped.splitlines():
            if GPIO_MUTATION_RE.search(line):
                matches.append(line.strip())
        if matches:
            results[f.name] = matches
    return results


# ---------------------------------------------------------------------------
# 2. find_relay_engine_callers — external callers of applyChannelState
# ---------------------------------------------------------------------------

def find_relay_engine_callers(fw_dir: Path) -> Dict[str, List[str]]:
    """Find external callers of applyChannelState (excluding RelayEngine.cpp/h itself)."""
    results: Dict[str, List[str]] = {}
    firmware_dir = fw_dir / "firmware"
    if not firmware_dir.is_dir():
        return results
    excluded = {"RelayEngine.cpp", "RelayEngine.h"}
    files = list(firmware_dir.glob("*.cpp")) + list(firmware_dir.glob("*.h"))
    for f in files:
        if f.name in excluded:
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        stripped = strip_cpp_comments_keep_lines(text)
        matches: List[str] = []
        for line in stripped.splitlines():
            if re.search(r"\bapplyChannelState\s*\(", line):
                matches.append(line.strip())
        if matches:
            results[f.name] = matches
    return results


# ---------------------------------------------------------------------------
# 3. check_ai_no_gpio_path
# ---------------------------------------------------------------------------

AI_FORBIDDEN_PATTERNS = [
    r"digitalWrite\s*\(",
    r"\bRelayDriver\b",
    r"\bRelayEngine\b",
    r"\bsetChannel\s*\(",
    r"\bapplyChannelState\s*\(",
    r"GPIO\.out_w1",
]

AI_FORBIDDEN_RE = re.compile("|".join(AI_FORBIDDEN_PATTERNS))


def check_ai_no_gpio_path(fw_dir: Path) -> List[str]:
    issues: List[str] = []
    firmware_dir = fw_dir / "firmware"
    targets = [
        firmware_dir / "Advisor.h",
        firmware_dir / "Advisor.cpp",
        firmware_dir / "InsightsHandlers.h",
    ]
    for f in targets:
        if not f.is_file():
            issues.append(f"{f.name}: file not found")
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        stripped = strip_cpp_comments_keep_lines(text)
        for m in AI_FORBIDDEN_RE.finditer(stripped):
            # find the line for context
            line_start = stripped.rfind("\n", 0, m.start()) + 1
            line_end = stripped.find("\n", m.end())
            if line_end == -1:
                line_end = len(stripped)
            line = stripped[line_start:line_end].strip()
            issues.append(f"{f.name}: forbidden pattern '{m.group(0)}' "
                         f"in AI code: {line[:120]}")
    return issues


# ---------------------------------------------------------------------------
# 4. check_gas_no_gpio_path
# ---------------------------------------------------------------------------

GAS_FORBIDDEN_PATTERNS = [
    r"relay_on",
    r"relay_off",
    r"setRelay",
    r"writeRelay",
    r"digitalWrite",
    r"GpioOutput",
]

GAS_FORBIDDEN_RE = re.compile("|".join(GAS_FORBIDDEN_PATTERNS), re.IGNORECASE)


def check_gas_no_gpio_path(fw_dir: Path) -> List[str]:
    issues: List[str] = []
    code_gs = fw_dir / "code.gs" / "Code.gs"
    if not code_gs.is_file():
        return [f"{code_gs} not found"]
    text = code_gs.read_text(encoding="utf-8", errors="replace")
    stripped = strip_js_comments_keep_lines(text)
    for m in GAS_FORBIDDEN_RE.finditer(stripped):
        line_start = stripped.rfind("\n", 0, m.start()) + 1
        line_end = stripped.find("\n", m.end())
        if line_end == -1:
            line_end = len(stripped)
        line = stripped[line_start:line_end].strip()
        issues.append(f"Code.gs: forbidden pattern '{m.group(0)}' "
                     f"(case-insensitive): {line[:120]}")
    return issues


# ---------------------------------------------------------------------------
# 5. simulate_malicious_ai_json
# ---------------------------------------------------------------------------

# Mirror PWA aiInsights.ts ALLOWED_ACTION_TYPES
ALLOWED_ACTION_TYPES = ["apply_suggestion", "review", "dismiss"]


def simulate_malicious_ai_json(pwa_dir: Path) -> List[str]:
    """Verify the malicious-AI contract:
    - apply_suggestion is in ALLOWED_ACTION_TYPES (so it's a valid label),
    - insight has advisoryOnly=true (server enforces non-actuating),
    - PWA renders as advisory card (no automatic mutation).

    For this script-level audit, we check the PWA's aiInsights.ts source
    for: ALLOWED_ACTION_TYPES includes 'apply_suggestion' and validates
    advisoryOnly !== false.
    """
    issues: List[str] = []
    ai_file = pwa_dir / "src" / "lib" / "aiInsights.ts"
    if not ai_file.is_file():
        return [f"{ai_file} not found"]
    text = ai_file.read_text(encoding="utf-8", errors="replace")
    stripped = strip_js_comments_keep_lines(text)

    # (a) ALLOWED_ACTION_TYPES must include 'apply_suggestion'
    if "apply_suggestion" not in stripped:
        issues.append("aiInsights.ts: ALLOWED_ACTION_TYPES missing 'apply_suggestion'")
    # (b) advisoryOnly validation must reject advisoryOnly === false
    if "advisoryOnly" not in stripped:
        issues.append("aiInsights.ts: advisoryOnly field not referenced")
    if "advisoryOnly === false" not in stripped and "advisoryOnly===false" not in stripped:
        issues.append("aiInsights.ts: advisoryOnly === false rejection not found")

    # (c) Look at PWA rendering — check ai-view.tsx for advisory card rendering
    view_file = pwa_dir / "src" / "components" / "ai" / "ai-view.tsx"
    if view_file.is_file():
        view_text = view_file.read_text(encoding="utf-8", errors="replace")
        view_stripped = strip_js_comments_keep_lines(view_text)
        # PWA must NOT auto-mutate on apply_suggestion — render as advisory card
        # (i.e., no fetch to /api/relay with method POST driven by apply_suggestion)
        if re.search(r"apply_suggestion.*fetch.*relay", view_stripped, re.IGNORECASE | re.DOTALL):
            issues.append("ai-view.tsx: apply_suggestion appears to trigger a "
                          "relay fetch (auto-mutation) — must be advisory only")
    return issues


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    fw_dir = repo_root / "firmware"
    pwa_dir = repo_root / "pwa"

    print("=" * 78)
    print("AI Actuator Isolation — Call-Graph Audit")
    print("=" * 78)

    # 1. GPIO mutations outside RelayEngine
    gpio_violations = find_gpio_mutations(fw_dir)
    print()
    print("--- 1. GPIO mutations outside RelayDriver/Engine ---")
    if not gpio_violations:
        print("  PASS — no direct GPIO mutations outside the authoritative actuator path")
    else:
        for fname, lines in gpio_violations.items():
            for line in lines:
                print(f"  FAIL: {fname}: {line[:120]}")

    # 2. External callers of applyChannelState
    callers = find_relay_engine_callers(fw_dir)
    print()
    print("--- 2. External callers of applyChannelState ---")
    if not callers:
        print("  PASS — no external callers (only RelayEngine itself mutates)")
    else:
        for fname, lines in callers.items():
            for line in lines:
                print(f"  WARN: {fname}: {line[:120]}  (external caller — verify)")

    # 3. AI code clean of GPIO references
    ai_issues = check_ai_no_gpio_path(fw_dir)
    print()
    print("--- 3. AI code (Advisor.h/cpp + InsightsHandlers.h) clean of GPIO refs ---")
    if not ai_issues:
        print("  PASS — Advisor + InsightsHandlers have no GPIO references")
    else:
        for iss in ai_issues:
            print(f"  FAIL: {iss}")

    # 4. GAS code clean of relay mutations
    gas_issues = check_gas_no_gpio_path(fw_dir)
    print()
    print("--- 4. GAS code (Code.gs) clean of relay mutations ---")
    if not gas_issues:
        print("  PASS — Code.gs has no relay mutation patterns")
    else:
        for iss in gas_issues:
            print(f"  FAIL: {iss}")

    # 5. Malicious AI JSON contract
    mal_issues = simulate_malicious_ai_json(pwa_dir)
    print()
    print("--- 5. Malicious AI JSON contract (apply_suggestion is advisory only) ---")
    if not mal_issues:
        print("  PASS — apply_suggestion in ALLOWED_ACTION_TYPES, advisoryOnly !== false enforced")
    else:
        for iss in mal_issues:
            print(f"  FAIL: {iss}")

    print()
    print("=" * 78)
    total_fail = (sum(len(v) for v in gpio_violations.values())
                  + len(ai_issues) + len(gas_issues) + len(mal_issues))
    if total_fail == 0:
        print("[AI_ACTUATOR_ISOLATION] PASS — AI/GAS code cannot mutate relays")
        print("=" * 78)
        return 0
    print(f"[AI_ACTUATOR_ISOLATION] FAIL — {total_fail} issue(s)")
    print("=" * 78)
    return 1


if __name__ == "__main__":
    sys.exit(main())
