#!/usr/bin/env python3
"""Lightweight, dependency-free checks for the project's Markdown documentation."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_FILES = (
    "README.md",
    "README.zh-CN.md",
    "docs/seat-architecture.zh-CN.md",
    "docs/seat-login1-contract.zh-CN.md",
)
MARKDOWN_LINK = re.compile(r"!?\[[^\]]*\]\((?:<([^>]+)>|([^\s)]+))(?:\s+[^)]*)?\)")


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)


def check_required_files() -> bool:
    ok = True
    for name in REQUIRED_FILES:
        path = ROOT / name
        if not path.is_file():
            fail(f"required documentation file is missing: {name}")
            ok = False
            continue
        if path.stat().st_size == 0:
            fail(f"documentation file is empty: {name}")
            ok = False
    return ok


def check_markdown(path: Path) -> bool:
    ok = True
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"{path.relative_to(ROOT)} is not valid UTF-8: {exc}")
        return False

    fence_char = None
    fence_length = 0
    for line_number, line in enumerate(content.splitlines(), start=1):
        match = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if not match:
            continue
        marker = match.group(1)
        char = marker[0]
        if fence_char is None:
            fence_char = char
            fence_length = len(marker)
        elif char == fence_char and len(marker) >= fence_length:
            fence_char = None
            fence_length = 0

    if fence_char is not None:
        fail(f"{path.relative_to(ROOT)} has an unclosed Markdown code fence")
        ok = False

    for match in MARKDOWN_LINK.finditer(content):
        target = match.group(1) or match.group(2) or ""
        parsed = urlsplit(target)
        if parsed.scheme or parsed.netloc or not parsed.path:
            continue
        decoded_path = unquote(parsed.path)
        if decoded_path.startswith("/"):
            candidate = ROOT / decoded_path.lstrip("/")
        else:
            candidate = path.parent / decoded_path
        if not candidate.exists():
            fail(
                f"{path.relative_to(ROOT)} links to missing local path: "
                f"{target}"
            )
            ok = False

    return ok


def main() -> int:
    ok = check_required_files()
    markdown_files = sorted(ROOT.rglob("*.md"))
    if not markdown_files:
        fail("no Markdown files were found")
        return 1

    for path in markdown_files:
        if not check_markdown(path):
            ok = False

    if not ok:
        return 1

    print(f"Documentation checks passed ({len(markdown_files)} Markdown files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
