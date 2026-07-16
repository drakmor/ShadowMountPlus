#!/usr/bin/env python3
"""Validate localization usage, source comments, coverage, and printf safety."""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "sm_l10n.h"
CATALOG_DIR = ROOT / "include" / "lang"
BASE_CATALOG = CATALOG_DIR / "en_us.inc"

ENUM_RE = re.compile(r"typedef enum\s*\{(.*?)SM_L10N_COUNT", re.DOTALL)
KEY_RE = re.compile(r"\b(SM_L10N_[A-Z0-9_]+)\b")
ENTRY_RE = re.compile(
    r"\[(SM_L10N_[A-Z0-9_]+)\]\s*=\s*"
    r"((?:\"(?:\\.|[^\"\\])*\"\s*)+)",
    re.DOTALL,
)
STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"')
PRINTF_RE = re.compile(
    r"%(?:[-+ #0]*)(?:\*|\d+)?(?:\.?(?:\*|\d+))?"
    r"(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn%]"
)
ENGLISH_COMMENT_RE = re.compile(
    r"/\* English: (.*?) \*/\s*\[(SM_L10N_[A-Z0-9_]+)\]", re.DOTALL
)


def localization_keys() -> list[str]:
    match = ENUM_RE.search(HEADER.read_text(encoding="utf-8"))
    if not match:
        raise ValueError(f"localization enum not found in {HEADER}")
    return KEY_RE.findall(match.group(1))


def read_catalog(path: Path) -> tuple[dict[str, str], list[str], int]:
    source_text = path.read_text(encoding="utf-8")
    entries: dict[str, str] = {}
    duplicates: list[str] = []
    assigned_string_count = 0
    for key, source in ENTRY_RE.findall(source_text):
        if key in entries:
            duplicates.append(key)
        tokens = STRING_RE.findall(source)
        assigned_string_count += len(tokens)
        entries[key] = "".join(ast.literal_eval(token) for token in tokens)

    without_comments = re.sub(r"/\*.*?\*/", "", source_text, flags=re.DOTALL)
    orphan_string_count = (
        len(STRING_RE.findall(without_comments)) - assigned_string_count
    )
    return entries, duplicates, orphan_string_count


def english_comment(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def read_english_comments(path: Path) -> tuple[dict[str, str], list[str]]:
    comments: dict[str, str] = {}
    duplicates: list[str] = []
    for source, key in ENGLISH_COMMENT_RE.findall(
        path.read_text(encoding="utf-8")
    ):
        if key in comments:
            duplicates.append(key)
        comments[key] = source
    return comments, duplicates


def printf_sequence(value: str) -> tuple[list[str], list[int]]:
    matches = list(PRINTF_RE.finditer(value))
    formats = [match.group(0) for match in matches]
    invalid_positions = [
        index
        for index, char in enumerate(value)
        if char == "%"
        and not any(match.start() <= index < match.end() for match in matches)
    ]
    return formats, invalid_positions


def read_source_code() -> str:
    sources: list[str] = []
    for directory in (ROOT / "src", ROOT / "include"):
        for path in directory.rglob("*"):
            if not path.is_file() or path.suffix not in {".c", ".h", ".S"}:
                continue
            if path == HEADER:
                continue
            sources.append(path.read_text(encoding="utf-8", errors="ignore"))
    return "\n".join(sources)


def main() -> int:
    keys = localization_keys()
    expected = set(keys)
    base, base_duplicates, base_orphans = read_catalog(BASE_CATALOG)
    source_code = read_source_code()
    errors: list[str] = []

    if base_duplicates:
        errors.append(f"{BASE_CATALOG.name}: duplicate keys: {base_duplicates}")
    if base_orphans:
        errors.append(
            f"{BASE_CATALOG.name}: {base_orphans} string literal(s) are not "
            "attached to a localization key"
        )
    missing_base = expected - base.keys()
    if missing_base:
        errors.append(
            f"{BASE_CATALOG.name}: missing base keys: {sorted(missing_base)}"
        )

    base_formats: dict[str, list[str]] = {}
    for key, value in base.items():
        formats, invalid_formats = printf_sequence(value)
        base_formats[key] = formats
        if invalid_formats:
            errors.append(
                f"{BASE_CATALOG.name}: {key}: invalid percent sign(s) at "
                f"{invalid_formats}"
            )

    for key in keys:
        if not re.search(rf"\b{re.escape(key)}\b", source_code):
            errors.append(f"{key}: localization key is not used by the code")

    catalogs = sorted(CATALOG_DIR.glob("*.inc"))
    if not catalogs:
        errors.append(f"no catalogs found in {CATALOG_DIR}")

    for path in catalogs:
        catalog, duplicates, orphans = read_catalog(path)
        missing = expected - catalog.keys()
        extra = catalog.keys() - expected
        if duplicates:
            errors.append(f"{path.name}: duplicate keys: {duplicates}")
        if orphans:
            errors.append(
                f"{path.name}: {orphans} string literal(s) are not attached "
                "to a localization key"
            )
        if missing:
            errors.append(f"{path.name}: missing keys: {sorted(missing)}")
        if extra:
            errors.append(f"{path.name}: unknown keys: {sorted(extra)}")

        if path != BASE_CATALOG:
            comments, duplicate_comments = read_english_comments(path)
            if duplicate_comments:
                errors.append(
                    f"{path.name}: duplicate English comments: "
                    f"{duplicate_comments}"
                )
            missing_comments = expected - comments.keys()
            if missing_comments:
                errors.append(
                    f"{path.name}: missing English comments: "
                    f"{sorted(missing_comments)}"
                )
            for key in keys:
                if key not in comments or key not in base:
                    continue
                expected_comment = english_comment(base[key])
                if comments[key] != expected_comment:
                    errors.append(
                        f"{path.name}: {key}: English comment is stale"
                    )

        for key in keys:
            if key not in base or key not in catalog:
                continue
            formats, invalid_formats = printf_sequence(catalog[key])
            if invalid_formats:
                errors.append(
                    f"{path.name}: {key}: invalid percent sign(s) at "
                    f"{invalid_formats}"
                )
            if formats != base_formats[key]:
                errors.append(
                    f"{path.name}: {key}: printf sequence {formats} != "
                    f"{base_formats[key]}"
                )
            if r"\n" in catalog[key]:
                errors.append(
                    f"{path.name}: {key}: contains a literal \\n instead of "
                    "a newline"
                )

    if errors:
        print("Localization validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(f"Validated {len(catalogs)} catalogs with {len(keys)} messages each.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
