# ESP-OpenClaw-Node

OpenClaw Native Node firmware for **M5Stack StickS3 / ESP32-S3**.

The project turns the ESP32 into a small OpenClaw Native Node. The Gateway owns Agent, LLM, Memory, IM, authorization, and routing; the device owns Wi-Fi, identity, bounded hardware commands, live Lua programming, and display rendering:

```text
OpenClaw Gateway
  ├─ Agent / LLM / Memory / authorization / routing
  └─ Invokes the Native Node over WebSocket
        ↓ Wi-Fi
M5Stack StickS3 / ESP32-S3
  └─ Device identity, display, audio, and bounded hardware commands
```

The ESP32 does not run the Agent or LLM. It executes only explicitly declared and validated firmware commands.

## Project identity

- Project name: **ESP-OpenClaw-Node**
- Target board: M5Stack StickS3
- MCU: ESP32-S3-PICO-1
- Display: ST7789 SPI, 135×240, RGB565
- Flash / PSRAM: 8 MB / 8 MB
- Protocol: OpenClaw Native Node protocol v4 with v3 Ed25519 Device Auth

## Implemented

### Native Node

- WebSocket transport, `connect.challenge`, and `hello-ok`
- Monocypher Ed25519 device identity
- NVS-persisted identity
- Stable Node ID derived from SHA-256 of the raw Ed25519 public key
- Gateway pairing, command-surface approval, and `node.invoke`
- `device.info`, `device.status`, and `device.network`
- `device.button.status`, `device.backlight`, and `device.restart`
- `audio.volume` and `audio.tone`
- Runtime Gateway URL, token, and device-family configuration without rebuilding
- A Wi-Fi-only provisioning portal with no embedded LLM or IM setup
- Live Lua scripts, file sandbox, and LVGL for rapid hardware and display experiments

### Native Node API reference

The firmware advertises these commands and executes them through OpenClaw Gateway `node.invoke`.

#### Device information and status

| API | Parameters | Description |
|---|---|---|
| `device.info` | `{}` | Device, chip, firmware, and capability information |
| `device.status` | `{}` | Runtime status |
| `device.network` | `{}` | Wi-Fi state, IP address, and gateway |
| `device.button.status` | `{}` | Button state |

#### Device control, display, and audio

| API | Parameters | Description |
|---|---|---|
| `device.backlight` | `{"level":0..100}` | Set backlight level |
| `device.restart` | `{}` | Restart the device |
| `device.screen.clear` | `{}` | Clear the notice layer and restore the system home screen |
| `device.screen.text` | `{"text":"..."}` | Show bounded Chinese text in the system UI notice area |
| `device.screen.fullscreen.enter` | `{}` | Enter the dedicated fullscreen page |
| `device.screen.fullscreen.text` | `{"text":"...","orientation":"portrait"}` | Show bounded fullscreen text |
| `device.screen.fullscreen.clear` | `{}` | Clear fullscreen content |
| `device.screen.fullscreen.exit` | `{}` | Exit fullscreen and restore the system home screen |
| `audio.volume` | Bounded volume parameters | Set volume |
| `audio.tone` | `{"frequencyHz":880,"durationMs":250}` | Play a tone |

Fullscreen text example:

```json
{
  "text": "Shanghai: sunny, 23–28°C",
  "orientation": "portrait"
}
```

`orientation: "landscape"` lays the fullscreen text page out sideways (the label is rotated 90° as an object), but the **panel itself stays portrait** — the LCD is not physically rotated. See "Display API notes".

#### Sandboxed file APIs

| API | Parameters | Description |
|---|---|---|
| `node.files.list` | Path parameters | List a sandbox directory |
| `node.files.read` | Path parameters | Read a file |
| `node.files.write` | Path and text content | Write a text file |
| `node.files.delete` | Path parameters | Delete a file |
| `node.files.copy` | `src_path`, `dst_path` | Copy a file |
| `node.files.move` | `src_path`, `dst_path` | Move a file |

File APIs retain the existing sandbox. `..` traversal is rejected; `/fatfs` is writable and `/system` is read-only.

#### Sandboxed Lua APIs

| API | Description |
|---|---|
| `node.lua.run` | Run an existing, path-validated device-side Lua script |
| `node.lua.run_async` | Run an existing Lua script asynchronously |
| `node.lua.jobs` | List asynchronous jobs |
| `node.lua.job` | Inspect one asynchronous job |
| `node.lua.stop` | Stop one asynchronous job |
| `node.lua.stop_all` | Stop all asynchronous jobs |

Arbitrary shell, terminal, `lua.eval`, and arbitrary command strings are not exposed.

## Display API notes

### Fonts and character coverage

Every string on screen goes through a single rendering path: LVGL `tiny_ttf` plus the
`NotoSansSC-Regular-sub.ttf` shipped with the firmware (~1.1 MB, roughly 3700 glyphs).
The system UI notice area, the fullscreen page, the clock, labels created through the Lua
`lvgl` module, and the `display.text` family in the Lua `display` module all share the same
font asset and the same size-to-glyph mapping, so the same string looks identical on every
path.

`text` is capped at 192 bytes for both `device.screen.text` and
`device.screen.fullscreen.text`; oversized or empty strings are rejected.

### Missing-glyph reporting

Text is queued asynchronously, so `"shown": true` / `"accepted": true` only means the command
was accepted — it cannot distinguish a real render from a row of missing-glyph boxes. Both
commands therefore report one extra field:

```json
{"command":"device.screen.text","shown":true,"missing_glyphs":0}
{"command":"device.screen.text","shown":true,"missing_glyphs":2,"first_missing":"U+1F9A9"}
{"command":"device.screen.fullscreen.text","accepted":true,"missing_glyphs":null}
```

- `missing_glyphs: 0` — every character has a glyph.
- `missing_glyphs: N` — N characters have no glyph and will draw as hollow boxes;
  `first_missing` gives the code point of the first one so it can be located directly.
- `missing_glyphs: null` — undetermined. This happens when the UI has not started yet (font not
  loaded) or on `fullscreen.enter/clear/exit`, which carry no text. **null is not 0** — do not
  read it as a pass.

The check runs against the same font the notice layer draws with, so the answer matches what
ends up on the panel.

### Emoji

`NotoEmoji-Regular-sub.ttf` (~260 KB, monochrome emoji subset) is chained behind the UI font
as `lv_font_t.fallback`. `lv_font_get_glyph_dsc()` walks that chain, so emoji are drawn as
ordinary glyphs and callers never have to substitute or pre-process them. The firmware no
longer embeds any emoji of its own - all displayed text comes from the Gateway - so whether a
given emoji renders is answered by the `missing_glyphs` field, not by documentation.

If the emoji font file is missing, text still renders and emoji degrade to a visible
missing-glyph box rather than silently disappearing.

### Font size

`font_size` on `display.text` and friends is a pixel height:

- `0` selects the default of 24.
- Other values are created and cached on demand, up to 16 sizes at a time; anything above
  128 is clamped to 128.

`device.screen.text` and `device.screen.fullscreen.text` take no size argument and both use
the notice size (16 px).

### Line breaks and orientation

- `\n` breaks the line and advances by the font line height; `\r` returns to the start of the
  line; `\t` advances by four space widths.
- `orientation` on `device.screen.fullscreen.text` accepts `"portrait"` (the default) and
  `"landscape"`, case-insensitively; `NULL` or an empty string means `"portrait"`. Any other
  value logs an `ESP_LOGW` and falls back to portrait rather than silently pretending to
  succeed.
- Landscape is implemented as a **90° transform on the label object**
  (`lv_obj_set_style_transform_rotation`), not by rotating the display controller. The panel
  is a fixed 135×240 portrait ST7789 and the SPI adapter latches its rotation at registration
  time, so changing LVGL's logical rotation at runtime would desynchronise the flush geometry
  and corrupt frames after the fullscreen page is torn down. Landscape therefore affects only
  the fullscreen text page; the panel orientation itself is unchanged. The label is re-laid
  out with its axes swapped and then rotated as a whole, so text wraps along the long edge.

### Adding characters

Fonts are packaged as character subsets, so **any character outside the subset renders as a
missing-glyph box**. After adding user-visible text (C code, Lua scripts, and so on) you need
to:

1. Run the coverage audit to see whether anything was missed:

   ```bash
   python3 tools/font_subset/scan_chars.py --check \
       --font application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf \
       --emoji-font application/edge_agent/fatfs_image/system/fonts/NotoEmoji-Regular-sub.ttf
   ```

   It walks `components/`, `application/` and both READMEs and lists every character that
   appears in **neither** character list, together with its `file:line` provenance. Add
   `--update` to append what is missing to the appropriate list.

   This check is wired into `.pre-commit-config.yaml` as the local hook
   `font-character-coverage`, so it runs automatically whenever a file under `components/`,
   `application/`, `tools/font_subset/` or either README changes. pre-commit installs the
   fontTools dependency itself; the only prerequisite is running `pre-commit install` once.

2. Re-run `tools/font_subset/build_fonts.py` to regenerate both font assets.
3. Re-flash the `system` partition.

See `tools/font_subset/README.md` for details. Note that the upstream Noto fonts are variable
fonts and must be pinned to `wght=400` before subsetting, otherwise they look fine on the
desktop but render incorrectly on the device.

Running `--check` without `--font` only proves the **lists** are complete, not that the
**font** contains those glyphs — a character can be listed yet still be absent from the font
(that is exactly how U+3000 IDEOGRAPHIC SPACE slipped through). Use `--font` for an
end-to-end answer.

## Not implemented

- Panel-level runtime portrait/landscape switching (the fullscreen text page's landscape mode is an object transform and does not change the panel orientation itself)
- Verified BLE HID pairing and host keyboard/mouse input
- Camera, screenshots, screen capture, or full computer control
- Arbitrary shell, terminal, or desktop scripting
- Native Node recording, audio streaming, STT/TTS, or continuous voice chat
- OTA firmware updates, remote flashing, or production-grade key rotation

## Build and flash

Verified toolchain: ESP-IDF 5.5.4 with Python 3.9.6.

```bash
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.9_env"
source "$IDF_PATH/export.sh"
cd application/edge_agent
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

Verify the board, chip, Flash, PSRAM, and serial port immediately before flashing. A successful flash is not feature acceptance; boot logs, Gateway reachability, command approval, invocation, and physical display output must be checked separately.

## Provisioning and security boundaries

The Gateway URL, optional token, and device family are stored through the local AP provisioning portal in NVS; leaving the URL empty keeps the Native Node disabled. There are no compile-time Gateway connection macros. Never place Gateway tokens, Wi-Fi passwords, API keys, private keys, or connection strings in Git, logs, or documentation. The identity seed is stored in NVS; the development configuration leaves Flash/NVS encryption disabled. Production devices must design key, backup, and recovery procedures before enabling encryption or using a secure element. Do not enable encryption and flash an existing device without that preparation.

Arbitrary `shell.exec`, `terminal.exec`, `lua.eval`, and arbitrary command strings are not exposed.

The provisioning portal itself is unauthenticated: the HTTP server listens on port 80, and anyone on the same network can read and rewrite the configuration, including repointing the Gateway URL at another server.

`GET /api/config` does not echo secrets. `wifi_password`, `ap_password`, and `openclaw_gateway_token` are reported as the fixed mask `********` when set; the WebUI sends that value back unchanged to mean "keep", and an empty string clears the slot. The real values never leave the device over HTTP.

Because the portal is unauthenticated, the first boot generates a 12-character random AP password, persists it to NVS, and prints it in the serial log, so the provisioning AP comes up as WPA2 rather than open. Clearing `ap_password` in the portal still yields an open AP for anyone who deliberately wants one.

Production deployments should still add authentication to the portal: the current state prevents secret disclosure but not reconfiguration.

## Repository layout

```text
components/openclaw_node/                 Native Node transport, identity, and dispatch
components/common/system_ui/               LVGL System UI and Unicode display
components/common/display_service/         LCD, LVGL adapter, and display lifecycle
components/claw_capabilities/cap_files/   Sandboxed file capability
components/claw_capabilities/cap_lua/     Lua script and job capability
application/edge_agent/                   ESP-IDF app and StickS3 integration
```

## Acceptance rule

`accepted: true`, `shown: true`, and `cleared: true` only mean that the firmware accepted a request. Display behavior requires a physical photo or video. API acceptance, build, flash, connection, command invocation, and visual output are separate acceptance records.

## Upstream note

The repository originated from Espressif ESP-Claw code, but its product name, GitHub documentation, and Native Node direction are now unified as **ESP-OpenClaw-Node**. The original project's brand logos and promotional graphics are not used as this project's product identity.

## License

See [`LICENSE`](./LICENSE).
