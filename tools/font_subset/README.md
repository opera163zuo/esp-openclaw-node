# Font Subset Tool

Generates the two TTF assets the firmware ships under
`application/edge_agent/fatfs_image/system/fonts/`:

| Asset | Role |
| --- | --- |
| `NotoSansSC-Regular-sub.ttf` | CJK + Latin UI font. Used by the LVGL system UI, the Lua `lvgl` module and the Lua `display` text APIs. |
| `NotoEmoji-Regular-sub.ttf` | Monochrome emoji font. Chained as `lv_font_t.fallback` behind the UI font so emoji render as real glyphs. |

## Dependency

```bash
python3 -m pip install fonttools
```

## Usage

The upstream Noto fonts are variable fonts. stb_truetype — the rasteriser inside
LVGL's `tiny_ttf` — does not apply variable instances reliably, so the script
first pins each source to a static `wght=400` instance and only then subsets it.
Do not feed a `[wght]` variable font straight to `fontTools.subset`: the result
looks correct in a desktop viewer but renders wrong (or not at all) on device.

Download the two upstream sources:

```bash
curl -L -o /tmp/NotoSansSC[wght].ttf \
    'https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf'
curl -L -o '/tmp/NotoEmoji[wght].ttf' \
    'https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf'
```

Then build both assets:

```bash
python3 tools/font_subset/build_fonts.py \
    --noto-sans-sc '/tmp/NotoSansSC[wght].ttf' \
    --noto-emoji '/tmp/NotoEmoji[wght].ttf' \
    --out-dir application/edge_agent/fatfs_image/system/fonts
```

Options:

- `--ui-text` / `--emoji-text`: character lists, defaulting to `character.txt`
  and `emoji.txt` next to the script.
- `--ui-output-name` / `--emoji-output-name`: output file names.
- `--keep-layout` / `--no-keep-layout`: keep or drop OpenType layout tables such
  as GSUB/GPOS. Keeping them is the default because the UI font relies on
  kerning for Latin text.
- `--out-dir`: required, and normally
  `application/edge_agent/fatfs_image/system/fonts`.

The script logs `requested/retained/missing` counts per asset and warns when the
result exceeds the size budget (1.5 MB for the UI font, 400 KB for emoji).

## Character Lists

`character.txt` is the source of truth for UI text. It must contain every
non-emoji code point the firmware can display, including the characters the
firmware itself emits from C code, Lua scripts and JSON rules. Missing entries
render as the missing-glyph box, so treat a new user-visible string as a reason
to update this file.

`emoji.txt` lists the emoji to keep. The list is deliberately broader than what
the firmware currently uses — notably `🦞` (U+1F99E), which appears in
`router_rules.json` and `claw_core_events.c` — because a monochrome emoji
subset is cheap compared to shipping a second font later.

## Character Coverage Audit

Both lists are hand-maintained, so they drift from what the repository actually
uses. `scan_chars.py` closes that gap: it walks the repository, collects every
code point that can reach the screen, and reports anything covered by neither
list.

```bash
# report
python3 tools/font_subset/scan_chars.py

# CI gate: exit 1 when something is uncovered
python3 tools/font_subset/scan_chars.py --check

# end-to-end: also verify the generated fonts really contain the characters
python3 tools/font_subset/scan_chars.py --check \
    --font application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf \
    --emoji-font application/edge_agent/fatfs_image/system/fonts/NotoEmoji-Regular-sub.ttf

# append newly discovered characters to the appropriate list
python3 tools/font_subset/scan_chars.py --update
```

Run the `--font` form after every `build_fonts.py` run. It is the only check
that proves the shipped assets cover the strings the firmware can display —
`--check` alone only proves the *lists* are complete, and a character can be
listed yet still absent from the font (see the U+3000 note below).

Options:

- `--root` / `--roots`: what to scan. Defaults to `components`, `application`,
  `README.md` and `README_EN.md`. Documentation prose is excluded on purpose:
  it is never rendered on the device and would drag in thousands of unrelated
  characters.
- `--include-ascii`: also report uncovered ASCII (0x20–0x7E). Off by default
  because the lists cover ASCII explicitly.
- `--list-files`: print every scanned file.
- `--ui-text` / `--emoji-text`: override the list paths.

Exit codes: `0` clean, `1` uncovered characters found (with `--check`), `2` usage
or I/O error.

### It runs automatically on commit

`.pre-commit-config.yaml` wires the end-to-end form in as a local hook
(`font-character-coverage`), so the audit runs whenever a file under
`components/`, `application/`, `tools/font_subset/` or either README changes. The hook
declares `language: python` with `additional_dependencies: [fonttools]`, which means
pre-commit installs fontTools into an isolated environment — no local setup beyond:

```bash
pre-commit install
```

Run it by hand against the whole tree with:

```bash
pre-commit run font-character-coverage --all-files
```

> **Keep the two predicates in step.** `build_fonts.is_drawable()` and
> `scan_chars.is_drawable()` must agree, or the audit will disagree with what
> actually lands in the font. The original `build_fonts.py` filtered with
> `not ch.isspace() or ch == ' '`, which silently dropped **U+3000 IDEOGRAPHIC
> SPACE** — a character the firmware uses in `claw_memory_utils.c`. It was
> present in `character.txt`, yet the shipped font had no glyph for it. Both
> scripts now filter on the Unicode category instead.

## Other Font Reduction Methods

- Text-based subsetting: keep only characters collected from actual UI strings.
  This tool uses this method and is the smallest option for fixed text.
- Unicode-range subsetting: keep a broad range such as CJK Unified Ideographs
  plus punctuation. Less precise but safer when runtime text is not fully known.
- Source string aggregation: scan Lua, C, frontend and config files to build a
  complete character set before subsetting. This is what `scan_chars.py` does —
  it audits the lists against the repository and can append what is missing,
  so `character.txt` no longer depends on someone remembering to update it.
- Bitmap or LVGL C fonts: convert glyphs into fixed-size bitmap data. Low
  runtime cost, but each font size needs its own generated asset — this is what
  the removed `esp_painter` path did, and why it could not render CJK.
- Font table trimming: remove hinting, names, bitmap strikes, variations or
  layout tables. Reduces size further but must be validated on the target.
- Split fonts: keep a small base font and load feature- or language-specific
  fonts separately. This is the approach used for the emoji companion, wired in
  through `lv_font_t.fallback`.
