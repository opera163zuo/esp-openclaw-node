# ESP-OpenClaw Native Node

`components/openclaw_node` is the transport and device-command layer for an
OpenClaw Native Node on ESP32.

## Boundary

The ESP32 firmware is intentionally **not an Agent**. It does not contain an
LLM runtime, memory/session manager, event router, scheduler, IM integration,
search provider, or skill manager. Those belong on the OpenClaw Gateway.

The device keeps the useful ESP-Claw runtime pieces that make it fun to work
with locally:

- the HTTP provisioning portal and Wi-Fi AP/STA setup;
- the live Lua scripting surface, including Lua hardware modules and LVGL;
- the local file sandbox used to store and run scripts;
- the system UI and TrueType Chinese/emoji rendering;
- serial allowlisted capability commands for field debugging.

## Native Node protocol

The component waits for `connect.challenge`, signs the v3 Ed25519 device-auth
payload, sends a `role: node` connect request over the OpenClaw WebSocket
protocol, and handles `node.invoke.request` events. Responses use
`node.invoke.result`.

The device identity is generated once and persisted in NVS. The private seed is
never returned through the public API. The current development configuration
leaves flash encryption disabled, so the NVS seed is not a production trust
anchor. Production devices should enable ESP flash/NVS encryption or use a
secure element. Do not toggle encryption on an existing device without first
backing up data and validating the key, provisioning, and recovery process.

The Gateway URL, optional token, and device family are runtime configuration
stored by `app_config` and entered from the local provisioning portal. There
are no Gateway credentials in source code. Leave the URL empty to keep the
Native Node connection disabled.

## Command surface

The application declares a fixed allowlist. Current command families are:

- device information, runtime status, network status, button status;
- backlight, restart, screen clear, notification text and full-screen text;
- audio volume and tone;
- sandboxed file list/read/write/delete/copy/move;
- existing Lua script execution, async jobs, job inspection and cancellation.

The file and Lua commands are path-checked. `..` traversal is rejected,
`/fatfs` is writable, and `/system` is read-only. The interface does not expose
arbitrary shell execution, terminal execution, `lua.eval`, or free-form command
strings.

## Provisioning and pairing

1. Power the board and join the printed provisioning AP if no usable station
   configuration is present.
2. Open the captive portal shown in the serial log.
3. Enter the Wi-Fi SSID/password.
4. Enter the OpenClaw Gateway WebSocket URL (`ws://` or `wss://`), token if the
   Gateway requires one, and an optional device-family label.
5. Save and restart. Once Wi-Fi has an IP, the Native Node connects in the
   background. Approve/pair the node and its declared command list on the
   Gateway.

The portal no longer asks for LLM credentials or IM platform credentials: the
Gateway owns those concerns.

## Dependencies

The manifest declares the local capability and board-manager dependencies used
by the command implementation. Keep these declarations in sync with the C
includes; do not rely on transitive dependencies from the application shell.
