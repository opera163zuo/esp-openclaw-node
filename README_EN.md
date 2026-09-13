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

Physical landscape switching is not complete. `orientation: "landscape"` must not be treated as evidence that the LCD rotated.

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
