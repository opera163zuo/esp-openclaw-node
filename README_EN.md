<div align="center">

  <a href="https://github.com/opera163zuo/esp-openclaw">
    <img src="./docs/src/assets/logos/logo.svg" alt="ESP-OpenClaw logo" width="45%" />
  </a>

  <h1>ESP-OpenClaw 🦞</h1>
  <h3>OpenClaw Native Node and Edge Agent project for ESP32</h3>

  <p>
    <a href="https://github.com/opera163zuo/esp-openclaw">
      <img src="https://img.shields.io/badge/platform-ESP32--S3-blue?style=flat-square" alt="ESP32-S3" />
    </a>
    <a href="./LICENSE">
      <img src="https://img.shields.io/github/license/opera163zuo/esp-openclaw?style=flat-square" alt="License" />
    </a>
  </p>

  <a href="./README.md">简体中文</a>
  |
  <a href="https://esp-claw.com/en/">ESP-Claw documentation</a>
  |
  <a href="./application/edge_agent/README.md">Build guide</a>

</div>

## Overview

ESP-OpenClaw is an OpenClaw Native Node project based on Espressif ESP-Claw, currently focused on the **M5Stack StickS3**.

The project uses a clear layered architecture:

```text
OpenClaw Gateway
  ├─ Agent / LLM / Memory / policy / communication routing
  └─ Invokes the Native Node over WebSocket
        ↓ Wi-Fi
M5Stack StickS3 / ESP32-S3
  └─ Device identity, hardware capabilities, and fixed commands
```

The ESP32 does not run the OpenClaw Agent, LLM, or social-platform logic. The Gateway handles understanding, orchestration, authorization, and routing; the device executes only declared and bounded commands.

> This repository still contains ESP-Claw's original edge-Agent, Capability, Lua, Memory, and configuration systems. Native Node is the main added direction and has been tested end to end on an M5Stack StickS3.

## Current status

### Implemented and verified on real hardware

- OpenClaw Native Node WebSocket connection and protocol v4 handshake
- `connect.challenge` and v3 Ed25519 Device Auth
- Persistent device identity stored in NVS
- Stable Node ID derived from SHA-256 of the raw Ed25519 public key
- Gateway pairing, command-surface approval, and `node.invoke` calls
- M5Stack StickS3: ESP32-S3-PICO-1, 8 MB Flash, 8 MB PSRAM
- Device information and status:
  - `device.info`
  - `device.status`
  - `device.network`
  - `device.button.status`
- Display and audio:
  - `device.backlight`
  - `device.screen.clear`
  - `audio.volume`
  - `audio.tone`
- Device control:
  - `device.restart`
- Sandboxed file capabilities:
  - `node.files.list`
  - `node.files.read`
  - `node.files.write`
  - `node.files.delete`
  - `node.files.copy`
  - `node.files.move`
- Sandboxed Lua scripts and jobs:
  - `node.lua.run`
  - `node.lua.run_async`
  - `node.lua.jobs`
  - `node.lua.job`
  - `node.lua.stop`
  - `node.lua.stop_all`

These commands have been tested end to end with a physical M5StickS3 and an OpenClaw Gateway. File tests use temporary files and clean them up afterward. Lua execution runs existing device-side `.lua` files; it is not arbitrary Lua-string evaluation.

### Not implemented yet

- BLE HID Native Node commands, host Bluetooth pairing, and keyboard/mouse input verification
- A restricted Native Node bridge for `cap_cli`
- Camera, screenshots, screen capture, and full computer control
- Arbitrary shell, arbitrary terminal, or desktop scripting
- Native Node recording, audio streaming, STT/TTS, and continuous voice conversation
- OTA firmware updates, remote flashing, and production-grade key rotation
- A more complete Native Node text/UI display API

BLE HID, screenshots, remote desktop, and full computer control must not be described as implemented at this stage.

## Native Node parameter examples

### Play a tone

```json
{
  "frequencyHz": 880,
  "durationMs": 250
}
```

Limits: `frequencyHz` from `100` to `4000`; `durationMs` from `1` to `2000`.

### Copy and move a file

```json
{
  "src_path": "/fatfs/source.txt",
  "dst_path": "/fatfs/copy.txt"
}
```

Copy and move require `src_path` and `dst_path`, not `source` and `destination`.

### Run an existing device-side Lua script

```json
{
  "path": "/system/skills/builtin_lua_modules/scripts/builtin/test/system_info.lua",
  "timeout_ms": 5000
}
```

Async example:

```json
{
  "path": "/system/skills/builtin_lua_modules/scripts/builtin/test/system_info.lua",
  "timeout_ms": 5000,
  "name": "protocol-test",
  "log_bytes": 2048
}
```

## Security boundaries

- File commands accept absolute paths only.
- Paths containing `..` are rejected.
- `/fatfs` is the writable data area; `/system` is the read-only firmware area.
- The original file sandbox and size limits remain in force.
- Lua may run only existing `.lua` files that pass path validation.
- `shell.exec`, `terminal.exec`, `lua.eval`, and arbitrary command strings are not exposed.
- A successful Native Node invocation is not the same as full computer control.
- Gateway URLs, tokens, Wi-Fi passwords, private keys, and other credentials must never be committed to Git.

## Hardware and build

Verified environment:

```text
Board: M5Stack StickS3
MCU: ESP32-S3-PICO-1
Flash: 8 MB
PSRAM: 8 MB
ESP-IDF: 5.5.4
Serial: /dev/cu.usbmodem101 (example)
```

After installing ESP-IDF:

```bash
cd application/edge_agent
source "$HOME/esp/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

Before flashing, verify the target board, chip, Flash, PSRAM, and serial port. Flash with:

```bash
idf.py -p /dev/cu.usbmodem101 flash
```

Native Node connection settings are disabled by default. A safe firmware should keep:

```c
#define OPENCLAW_NODE_GATEWAY_URL ""
#define OPENCLAW_NODE_GATEWAY_TOKEN ""
```

A live Gateway test requires a temporary local, uncommitted configuration. After testing, restore empty URL/token settings and return the Gateway to loopback binding.

## Repository layout

```text
components/openclaw_node/                 Native Node transport, identity, and dispatch
components/claw_capabilities/cap_files/   Sandboxed file Capability
components/claw_capabilities/cap_lua/     Lua script and async-job Capability
components/claw_capabilities/cap_cli/     Original restricted ESP Console Capability
application/edge_agent/                   M5Stack StickS3 app and board integration
components/lua_modules/                   Original Lua modules
```

## GitHub changes

The main Native Node changes have been committed to `master`, including:

- Native Node WebSocket transport and OpenClaw handshake
- Monocypher Ed25519 identity and NVS persistence
- Device information, status, network, button, backlight, restart, audio, and screen commands
- Native Node mappings for sandboxed file and Lua Capabilities
- ESP-IDF Component Manifest and dependency fixes
- Real M5Stack StickS3 / Gateway integration fixes

Temporary Gateway URLs, tokens, and Wi-Fi credentials are not stored in GitHub.

## Upstream and acknowledgements

This project is based on [Espressif ESP-Claw](https://github.com/espressif/esp-claw). Thanks for its Agent Loop, Capability, Lua, Memory, board support, and documentation work.

The Native Node direction is inspired by [OpenClaw](https://github.com/openclaw/openclaw).

## License

See [`LICENSE`](./LICENSE).
