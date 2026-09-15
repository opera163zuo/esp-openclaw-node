#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Audit the font subset character lists against what the repository actually uses.

``build_fonts.py`` subsets the upstream fonts to whatever is listed in
``character.txt`` / ``emoji.txt``. Those lists are maintained by hand, so they
drift: a new UI string, a Lua skill or a README example can introduce a
character the shipped font has no glyph for, and the device then draws a
missing-glyph box with no build-time warning. That is exactly how ``°``
(U+00B0) and ``–`` (U+2013) came to be missing from a README example that the
firmware ships.

This script closes that gap. It walks the repository, collects every code point
that can reach the screen, and reports anything covered by neither character
list. With ``--font`` it goes one step further and verifies the generated font
assets really contain the characters they were asked to keep.

Usage:

    # report only
    python3 tools/font_subset/scan_chars.py

    # CI gate: exit 1 when something is uncovered
    python3 tools/font_subset/scan_chars.py --check

    # full end-to-end check against the shipped assets
    python3 tools/font_subset/scan_chars.py --check \\
        --font application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf \\
        --emoji-font application/edge_agent/fatfs_image/system/fonts/NotoEmoji-Regular-sub.ttf

    # append newly discovered characters to the appropriate list
    python3 tools/font_subset/scan_chars.py --update
"""

from __future__ import annotations

import argparse
import logging
import sys
import unicodedata
from pathlib import Path

LOGGER = logging.getLogger('scan_chars')

# Only files that can actually put text on the screen are scanned. Docs prose is
# excluded by default because it is never rendered on the device and would drag
# in thousands of unrelated characters.
DEFAULT_ROOTS = ('components', 'application', 'README.md', 'README_EN.md')

TEXT_SUFFIXES = {
    '.c', '.h', '.cc', '.cpp', '.hpp', '.inc', '.ipp',
    '.lua',
    '.json', '.jsonl', '.json5',
    '.yaml', '.yml',
    '.md', '.mdx',
    '.py',
    '.txt', '.cfg', '.conf', '.ini',
    '.cmake', '.sh', '.ld',
}

TEXT_FILENAMES = {
    'Kconfig', 'Kconfig.projbuild', 'CMakeLists.txt', 'Makefile',
}

# Build outputs, vendored dependencies and the character lists themselves.
SKIP_DIRS = {
    '.git', '.workbuddy-ai', '.venv', 'venv', '__pycache__', 'node_modules',
    'build', 'managed_components', 'dist', '.cache', '.espressif', 'gen_codes',
}

# Binary-ish assets that happen to share a text suffix or carry no suffix.
SKIP_SUFFIXES = {
    '.ttf', '.otf', '.woff', '.woff2', '.bin', '.elf', '.a', '.o', '.obj',
    '.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.ico', '.pdf',
    '.zip', '.gz', '.xz', '.tar', '.pyc', '.pyo', '.so', '.dylib',
}

MAX_FILE_BYTES = 2 * 1024 * 1024
MAX_EXAMPLES_PER_CHAR = 3

# Ranges routed to the emoji subset. Used only by --update to decide which list a
# newly discovered character belongs in; coverage is always tested against both.
EMOJI_RANGES = (
    (0x1F000, 0x1FAFF),   # pictographs, emoticons, transport, supplemental symbols
    (0x2600, 0x27BF),     # miscellaneous symbols and dingbats
    (0x2B00, 0x2BFF),     # miscellaneous symbols and arrows
    (0x2300, 0x23FF),     # miscellaneous technical (hourglass, stopwatch, ...)
    (0xFE00, 0xFE0F),     # variation selectors
    (0x200D, 0x200D),     # zero-width joiner
)


class ScanError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    repo_root = here.parent.parent
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', default=str(repo_root),
                        help='Repository root to scan (default: the repository containing this script).')
    parser.add_argument('--roots', nargs='+', default=list(DEFAULT_ROOTS),
                        help='Paths under --root to scan (default: %(default)s).')
    parser.add_argument('--ui-text', default=str(here / 'character.txt'),
                        help='UI character list (default: character.txt next to this script).')
    parser.add_argument('--emoji-text', default=str(here / 'emoji.txt'),
                        help='Emoji character list (default: emoji.txt next to this script).')
    parser.add_argument('--font', help='Generated UI font to verify against.')
    parser.add_argument('--emoji-font', help='Generated emoji font to verify against.')
    parser.add_argument('--check', action='store_true',
                        help='Exit non-zero when any used character is uncovered.')
    parser.add_argument('--update', action='store_true',
                        help='Append uncovered characters to the appropriate list.')
    parser.add_argument('--include-ascii', action='store_true',
                        help='Also report uncovered ASCII characters (0x20-0x7E).')
    parser.add_argument('--list-files', action='store_true',
                        help='Print every file that was scanned.')
    return parser.parse_args()


def read_character_list(path: Path) -> set[int]:
    """Read a character list. The file is a bag of characters, not a codepoint list."""
    try:
        text = path.read_text(encoding='utf-8')
    except FileNotFoundError:
        raise ScanError(f'missing character list: {path}') from None
    except UnicodeDecodeError as exc:
        raise ScanError(f'character list must be UTF-8: {path}: {exc}') from None
    except OSError as exc:
        raise ScanError(f'read character list failed: {path}: {exc}') from None

    # Mirrors build_fonts.is_drawable so the two tools agree on coverage. Note
    # that whitespace is kept: U+3000 IDEOGRAPHIC SPACE is a real CJK glyph the
    # firmware uses, and filtering on str.isspace() would hide it.
    codepoints = {ord(ch) for ch in text if is_drawable(ch)}
    if not codepoints:
        raise ScanError(f'character list is empty: {path}')
    return codepoints


def is_drawable(ch: str) -> bool:
    """False for control/format/surrogate/private-use characters (no visible glyph).

    Mirrors ``build_fonts.is_drawable``; keep the two in step.
    """
    cp = ord(ch)
    if cp < 0x20:
        return False
    return unicodedata.category(ch) not in ('Cc', 'Cf', 'Cs', 'Co')


def wants_emoji_list(cp: int) -> bool:
    return any(low <= cp <= high for low, high in EMOJI_RANGES)


def iter_candidate_files(root: Path, roots: list[str]):
    seen: set[Path] = set()
    for entry in roots:
        target = (root / entry).resolve()
        if not target.exists():
            LOGGER.warning('scan root does not exist, skipping: %s', entry)
            continue
        candidates = [target] if target.is_file() else target.rglob('*')
        for path in candidates:
            if not path.is_file() or path in seen:
                continue
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            if path.suffix.lower() in SKIP_SUFFIXES:
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in TEXT_FILENAMES:
                continue
            try:
                if path.stat().st_size > MAX_FILE_BYTES:
                    LOGGER.debug('skipping oversized file: %s', path)
                    continue
            except OSError:
                continue
            seen.add(path)
            yield path


def scan_repository(root: Path, roots: list[str], include_ascii: bool) -> tuple[dict[int, list[str]], int, list[Path]]:
    """Return (codepoint -> example locations, files scanned, files scanned list)."""
    used: dict[int, list[str]] = {}
    scanned: list[Path] = []

    for path in sorted(iter_candidate_files(root, roots)):
        try:
            text = path.read_text(encoding='utf-8')
        except (UnicodeDecodeError, OSError):
            continue
        scanned.append(path)
        rel = path.relative_to(root)
        for lineno, line in enumerate(text.splitlines(), start=1):
            for ch in line:
                if not is_drawable(ch):
                    continue
                cp = ord(ch)
                if cp < 0x80 and not include_ascii:
                    continue
                bucket = used.setdefault(cp, [])
                if len(bucket) < MAX_EXAMPLES_PER_CHAR:
                    bucket.append(f'{rel}:{lineno}')
    return used, len(scanned), scanned


def codepoints_in_font(path: Path) -> set[int]:
    try:
        from fontTools.ttLib import TTFont, TTLibError
    except ImportError:
        raise ScanError('fontTools is required for --font; pip install fonttools') from None
    try:
        font = TTFont(path)
    except (TTLibError, OSError) as exc:
        raise ScanError(f'open font failed: {path}: {exc}') from None
    covered: set[int] = set()
    for table in font['cmap'].tables if 'cmap' in font else []:
        if table.isUnicode():
            covered.update(table.cmap.keys())
    return covered


def describe(cp: int) -> str:
    try:
        name = unicodedata.name(chr(cp))
    except ValueError:
        name = '<unnamed>'
    return f'U+{cp:04X} {chr(cp)!r:<8} {name}'


def append_to_list(path: Path, codepoints: list[int]) -> None:
    """Append characters on their own line.

    The list is read as a bag of characters, so a comment here would be absorbed
    into the subset as literal glyphs. Only the characters themselves go in.
    """
    try:
        with path.open('a', encoding='utf-8') as handle:
            handle.write('\n')
            handle.write(' '.join(chr(cp) for cp in codepoints))
            handle.write('\n')
    except OSError as exc:
        raise ScanError(f'append to character list failed: {path}: {exc}') from None


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=logging.INFO, format='%(levelname)s: %(message)s')

    root = Path(args.root).resolve()
    ui_path = Path(args.ui_text).resolve()
    emoji_path = Path(args.emoji_text).resolve()

    try:
        ui_chars = read_character_list(ui_path)
        emoji_chars = read_character_list(emoji_path)
        used, file_count, scanned = scan_repository(root, args.roots, args.include_ascii)
    except ScanError as exc:
        LOGGER.error('scan_chars.py: error: %s', exc)
        return 2

    if args.list_files:
        for path in scanned:
            print(path.relative_to(root))

    listed = ui_chars | emoji_chars
    missing = sorted(cp for cp in used if cp not in listed)

    LOGGER.info('scanned %d files under %s', file_count, root)
    LOGGER.info('character.txt: %d characters | emoji.txt: %d characters | union: %d',
                len(ui_chars), len(emoji_chars), len(listed))
    LOGGER.info('used by the repository: %d distinct characters', len(used))

    if missing:
        LOGGER.warning('%d used character(s) are in neither list:', len(missing))
        for cp in missing:
            target = 'emoji.txt' if wants_emoji_list(cp) else 'character.txt'
            # Routed through the logger rather than print() so the details stay
            # ordered with their header when both streams are piped together.
            LOGGER.warning('  %s  -> %s   %s', describe(cp), target, ', '.join(used[cp]))
    else:
        LOGGER.info('every used character is covered by a character list')

    if args.update and missing:
        ui_add = [cp for cp in missing if not wants_emoji_list(cp)]
        emoji_add = [cp for cp in missing if wants_emoji_list(cp)]
        if ui_add:
            append_to_list(ui_path, ui_add)
            LOGGER.info('appended %d character(s) to %s', len(ui_add), ui_path.name)
        if emoji_add:
            append_to_list(emoji_path, emoji_add)
            LOGGER.info('appended %d character(s) to %s', len(emoji_add), emoji_path.name)
        LOGGER.info('re-run build_fonts.py to regenerate the font assets')

    font_gaps: list[int] = []
    if args.font:
        try:
            covered = codepoints_in_font(Path(args.font).resolve())
            if args.emoji_font:
                covered |= codepoints_in_font(Path(args.emoji_font).resolve())
        except ScanError as exc:
            LOGGER.error('scan_chars.py: error: %s', exc)
            return 2
        font_gaps = sorted(cp for cp in used if cp not in covered)
        LOGGER.info('shipped fonts cover %d characters', len(covered))
        if font_gaps:
            LOGGER.warning('%d used character(s) are absent from the shipped fonts:', len(font_gaps))
            for cp in font_gaps:
                LOGGER.warning('  %s  %s', describe(cp), ', '.join(used[cp]))
        else:
            LOGGER.info('every used character is present in the shipped fonts')

    if args.check and (missing or font_gaps):
        return 1
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
