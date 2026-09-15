#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Build the firmware font assets from upstream variable fonts.

Two assets are produced:

* ``NotoSansSC-Regular-sub.ttf`` - the CJK + Latin UI font used by the LVGL
  system UI, the Lua ``lvgl`` module and the Lua ``display`` text APIs.
* ``NotoEmoji-Regular-sub.ttf`` - a monochrome emoji font wired in as the
  ``fallback`` of the UI font, so emoji render as real glyphs instead of
  missing-glyph boxes.

Both upstream fonts ship as variable fonts. LVGL's tiny_ttf rasteriser relies on
stb_truetype, which does not apply variable-font instances reliably, so each
source is first pinned to a static ``wght=400`` instance and then subset to the
characters listed in the corresponding text file.

Usage:

    python3 tools/font_subset/build_fonts.py \
        --noto-sans-sc /path/to/NotoSansSC[wght].ttf \
        --noto-emoji /path/to/NotoEmoji[wght].ttf \
        --out-dir application/edge_agent/fatfs_image/system/fonts
"""

from __future__ import annotations

import argparse
import logging
import sys
import unicodedata
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont, TTLibError
from fontTools.varLib import instancer

LOGGER = logging.getLogger('build_fonts')

STATIC_WEIGHT = 400
FONT_SIZE_WARN_BYTES = 1_500_000
EMOJI_SIZE_WARN_BYTES = 400_000


class BuildFontError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise BuildFontError(message)


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--noto-sans-sc', required=True,
                        help='Upstream NotoSansSC variable font (TTF/OTF).')
    parser.add_argument('--noto-emoji', required=True,
                        help='Upstream NotoEmoji variable font (TTF/OTF).')
    parser.add_argument('--out-dir', required=True,
                        help='Directory that receives the generated subset fonts.')
    parser.add_argument('--ui-text', default=str(here / 'character.txt'),
                        help='UTF-8 text file listing UI characters to keep.')
    parser.add_argument('--emoji-text', default=str(here / 'emoji.txt'),
                        help='UTF-8 text file listing emoji to keep.')
    parser.add_argument('--ui-output-name', default='NotoSansSC-Regular-sub.ttf')
    parser.add_argument('--emoji-output-name', default='NotoEmoji-Regular-sub.ttf')
    parser.add_argument('--keep-layout', action=argparse.BooleanOptionalAction, default=True,
                        help='Keep OpenType layout tables such as GSUB/GPOS.')
    return parser.parse_args()


def is_drawable(ch: str) -> bool:
    """True for characters that need a real glyph in the subset.

    Whitespace is deliberately *not* excluded. U+3000 IDEOGRAPHIC SPACE is a
    legitimate CJK glyph that the firmware uses for punctuation segmentation
    (``claw_memory_utils.c``), and an earlier ``ch.isspace()`` filter silently
    dropped it even though ``character.txt`` listed it - the character was in
    the list, yet the shipped font had no glyph for it.

    ``scan_chars.py`` mirrors this predicate; keep the two in step.
    """
    cp = ord(ch)
    if cp < 0x20:
        return False
    return unicodedata.category(ch) not in ('Cc', 'Cf', 'Cs', 'Co')


def read_characters(path: Path) -> list[int]:
    try:
        text = path.read_text(encoding='utf-8')
    except FileNotFoundError:
        fail(f'missing character list: {path}')
    except UnicodeDecodeError as exc:
        fail(f'character list must be UTF-8: {path}: {exc}')
    except OSError as exc:
        fail(f'read character list failed: {path}: {exc}')

    codepoints = sorted({ord(ch) for ch in text if is_drawable(ch)})
    if not codepoints:
        fail(f'character list is empty: {path}')
    return codepoints


def load_font(path: Path) -> TTFont:
    if not path.is_file():
        fail(f'missing font file: {path}')
    try:
        return TTFont(path)
    except TTLibError as exc:
        fail(f'open font failed: {path}: {exc}')
    except OSError as exc:
        fail(f'read font failed: {path}: {exc}')


def pin_static_instance(font: TTFont, weight: int) -> TTFont:
    """Return a static instance of a variable font, or the font unchanged."""
    if 'fvar' not in font:
        return font
    try:
        return instancer.instantiateVariableFont(font, {'wght': weight}, inplace=False)
    except Exception as exc:  # noqa: BLE001 - surface any instancer failure as our error type
        fail(f'pin wght={weight} failed: {exc}')


def supported_codepoints(font: TTFont) -> set[int]:
    supported: set[int] = set()
    for table in font['cmap'].tables if 'cmap' in font else []:
        if table.isUnicode():
            supported.update(table.cmap.keys())
    return supported


def build_options(keep_layout: bool) -> subset.Options:
    options = subset.Options()
    options.flavor = None
    options.with_zopfli = False
    options.recalc_bounds = True
    options.recalc_timestamp = False
    options.canonical_order = True
    options.notdef_outline = True
    if keep_layout:
        options.layout_features = ['*']
    else:
        options.layout_features = []
        options.drop_tables += ['GSUB', 'GPOS']
    return options


def build_subset(source: Path, characters: list[int], output: Path, keep_layout: bool,
                 label: str) -> None:
    font = pin_static_instance(load_font(source), STATIC_WEIGHT)
    supported = supported_codepoints(font)
    retained = [cp for cp in characters if cp in supported]
    missing = [cp for cp in characters if cp not in supported]

    if not retained:
        fail(f'{label}: none of the requested characters exist in {source.name}')

    subsetter = subset.Subsetter(options=build_options(keep_layout))
    subsetter.populate(unicodes=retained)
    subsetter.subset(font)

    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        font.save(output)
    except OSError as exc:
        fail(f'write output font failed: {output}: {exc}')

    size = output.stat().st_size
    LOGGER.info('%s: requested=%d retained=%d missing=%d size=%d bytes -> %s',
                label, len(characters), len(retained), len(missing), size, output.name)
    if missing:
        preview = ' '.join(f'U+{cp:04X}' for cp in missing[:20])
        suffix = '' if len(missing) <= 20 else f' (+{len(missing) - 20} more)'
        LOGGER.warning('%s: upstream font lacks %d requested characters: %s%s',
                       label, len(missing), preview, suffix)
    return size


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=logging.INFO, format='%(levelname)s: %(message)s')

    out_dir = Path(args.out_dir).resolve()
    ui_chars = read_characters(Path(args.ui_text).resolve())
    emoji_chars = read_characters(Path(args.emoji_text).resolve())

    ui_size = build_subset(Path(args.noto_sans_sc).resolve(), ui_chars,
                           out_dir / args.ui_output_name, args.keep_layout, 'ui font')
    emoji_size = build_subset(Path(args.noto_emoji).resolve(), emoji_chars,
                              out_dir / args.emoji_output_name, args.keep_layout, 'emoji font')

    if ui_size > FONT_SIZE_WARN_BYTES:
        LOGGER.warning('ui font is %d bytes, above the %d byte budget',
                       ui_size, FONT_SIZE_WARN_BYTES)
    if emoji_size > EMOJI_SIZE_WARN_BYTES:
        LOGGER.warning('emoji font is %d bytes, above the %d byte budget',
                       emoji_size, EMOJI_SIZE_WARN_BYTES)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BuildFontError as exc:
        LOGGER.error('build_fonts.py: error: %s', exc)
        sys.exit(1)
