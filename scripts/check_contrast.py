#!/usr/bin/env python3
"""Verify Aurora's theme colors against WCAG AA contrast ratios.

The dark theme used to render muted and disabled text at ~2.5:1 against its own
surfaces (dark-on-dark). This script parses the hex literals out of
src/ui/Theme.cpp and asserts every foreground/background pair the chrome
actually draws clears its WCAG AA threshold:

  * body / secondary text  >= 4.5:1
  * large text (page titles, 24px) and UI borders >= 3.0:1

Run from the repo root:  python3 scripts/check_contrast.py
Exit code 0 = every pair passes.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

THEME_FILE = Path(__file__).resolve().parent.parent / "src" / "ui" / "Theme.cpp"

# (foreground token, background token, minimum ratio, what it is)
PAIRS = [
    ("text", "background", 4.5, "primary text on window background"),
    ("text", "surface", 4.5, "primary text on toolbar/status surface"),
    ("text", "surface_alt", 4.5, "primary text on inputs and list rows"),
    ("text", "tab_active_bg", 4.5, "active tab label"),
    ("text_muted", "background", 4.5, "muted text on window background"),
    ("text_muted", "surface", 4.5, "muted text on toolbar (nav icons)"),
    ("text_muted", "tab_inactive_bg", 4.5, "inactive tab label"),
    ("text_muted", "tab_hover_bg", 4.5, "hovered tab label"),
    ("text_faint", "surface", 4.5, "disabled text on surfaces"),
    ("text_faint", "background", 4.5, "disabled text on background"),
    ("text", "omnibox_bg", 4.5, "omnibox text"),
    ("accent", "background", 3.0, "accent on background (borders, +)"),
    ("accent", "surface", 3.0, "accent on surface"),
    ("error", "surface", 3.0, "error color on surface"),
    ("warning", "surface", 3.0, "warning color on surface"),
    ("success", "surface", 3.0, "success color on surface"),
    ("border", "background", 1.4, "border separation from background"),
]


def relative_luminance(hex_color: str) -> float:
    h = hex_color.lstrip("#")
    channels = [int(h[i : i + 2], 16) / 255 for i in (0, 2, 4)]
    linear = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    r, g, b = linear
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a: str, b: str) -> float:
    la, lb = relative_luminance(a), relative_luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def parse_themes(source: str) -> dict[str, dict[str, str]]:
    """Extract {theme_name: {token: '#RRGGBB'}} from Theme.cpp."""
    themes: dict[str, dict[str, str]] = {}
    current: str | None = None
    aliases: dict[str, str] = {}
    for line in source.splitlines():
        fn = re.match(r"\s*Theme\s+(\w+)_theme\(\)", line)
        if fn:
            current = fn.group(1)
            themes[current] = {}
            aliases = {}
            continue
        if current is None:
            continue
        hexed = re.match(r'\s*c\.(\w+)\s*=\s*QColor\("(#[0-9A-Fa-f]{6})"\)', line)
        if hexed:
            themes[current][hexed.group(1)] = hexed.group(2)
            continue
        alias = re.match(r"\s*c\.(\w+)\s*=\s*c\.(\w+);", line)
        if alias:
            aliases[alias.group(1)] = alias.group(2)
    # Resolve one level of alias chains (c.tab_inactive_bg = c.surface, etc).
        for target, source_token in list(aliases.items()):
            if source_token in themes[current]:
                themes[current][target] = themes[current][source_token]
    return themes


def main() -> int:
    source = THEME_FILE.read_text(encoding="utf-8")
    themes = parse_themes(source)
    if not themes:
        print(f"error: no themes parsed from {THEME_FILE}", file=sys.stderr)
        return 2

    failures = 0
    for name, tokens in sorted(themes.items()):
        print(f"\n{name} theme")
        print("-" * 68)
        for fg, bg, minimum, label in PAIRS:
            if fg not in tokens or bg not in tokens:
                print(f"  SKIP  {fg} on {bg} (token not defined)")
                continue
            ratio = contrast(tokens[fg], tokens[bg])
            ok = ratio >= minimum
            failures += 0 if ok else 1
            print(
                f"  {'PASS' if ok else 'FAIL'}  {ratio:5.2f}:1 "
                f"(min {minimum:.1f})  {label}  [{tokens[fg]} on {tokens[bg]}]"
            )

    print()
    if failures:
        print(f"{failures} contrast check(s) failed")
        return 1
    print("all contrast checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
