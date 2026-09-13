# ESP-OpenClaw-Node

OpenClaw Native Node firmware for **M5Stack StickS3 / ESP32-S3**.

The project separates the OpenClaw Gateway from the embedded execution layer:

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

### Display

- Existing LVGL + Tiny TTF + NotoSansSC Chinese rendering path
- `device.screen.text` for bounded Chinese notices
- `device.screen.fullscreen.enter`
- `device.screen.fullscreen.text`
- `device.screen.fullscreen.clear`
- `device.screen.fullscreen.exit`
- Automatic line wrapping for Chinese text
- Bounded weather/common-symbol graphic mapping to avoid missing-glyph boxes
- All LVGL object operations run through the System UI task

The stable display orientation is currently **portrait**. Runtime LCD landscape switching is not complete. An accepted `orientation: landscape` parameter or API response is not evidence of physical rotation. A correct implementation must rebuild the ST7789 panel path, LVGL adapter, frame buffers, flush geometry, and UI pages together.

### Sandboxed files and Lua

- `node.files.list/read/write/delete/copy/move`
- `node.lua.run/run_async/jobs/job/stop/stop_all`
- Existing filesystem sandbox and `..` traversal rejection
- `/fatfs` writable and `/system` read-only
- Lua is limited to existing, path-validated device-side scripts

## Not implemented

- Stable runtime portrait/landscape switching
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

## Security boundaries

The default firmware has no Gateway credentials:

```c
#define OPENCLAW_NODE_GATEWAY_URL ""
#define OPENCLAW_NODE_GATEWAY_TOKEN ""
```

Temporary integration credentials must stay in a local, uncommitted overlay. Never place Gateway tokens, Wi-Fi passwords, API keys, private keys, or connection strings in Git, logs, or documentation. After live testing, restore empty URL/token settings and Gateway `bind=loopback`.

Arbitrary `shell.exec`, `terminal.exec`, `lua.eval`, and arbitrary command strings are not exposed.

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
