# ESP-OpenClaw-Node application guide

This is the main application in the repository. Despite the directory name, it
runs no on-device Agent: the device is an OpenClaw Native Node that executes
commands sent by a remote Gateway.

## FATFS Image Layout

Source content for all FAT partitions lives under a single tree, with one
subdirectory per partition:

```text
application/edge_agent/fatfs_image/
├── storage/   # → storage partition (writable, mounted at /fatfs)
└── system/    # → system partition (read-only seed, mounted at /system)
```

Each subdirectory is copied into its own build-time staging directory:

```text
application/edge_agent/build/fatfs_image/        # storage staging
application/edge_agent/build/system_fs_image/    # system staging
```

Each board can also provide optional board-specific FATFS content under its own board directory. This content is overlaid onto the `system` partition:

```text
application/edge_agent/boards/<vendor>/<board>/fatfs_image/
```

During the build, `application/edge_agent/CMakeLists.txt` first copies the base `fatfs_image/system/` directory into the system staging dir, then copies the selected board's `fatfs_image/` directory if it exists. The selected board path comes from the generated `components/gen_bmgr_codes/CMakeLists.txt`, which is produced by `idf.py gen-bmgr-config`.

If a board-specific file has the same relative path as a base system file, the board-specific file overwrites the base file in `build/system_fs_image/`. This lets a board replace firmware-baked defaults such as scripts and static assets without changing the shared base image. Board `fatfs_image/` content targets the SYSTEM image only; hidden board folders are not considered.

Built-in Lua scripts and docs are synced into `build/system_fs_image/` so they end up on the read-only system partition; the writable storage partition can be reformatted at runtime and re-seeded from `/system` without losing them.


## Quick Start

### Prerequisites

- ESP-IDF is installed and exported
- `ESP-IDF v5.5.4` is recommended

```bash
. <your-esp-idf-path>/export.sh
```

### Configuration

To make `esp-board-manager` easier to use, first install the helper package with `pip install esp-bmgr-assist`. You only need to do this once in a given ESP-IDF environment.

1. Generate board support files:

```bash
cd application/edge_agent
idf.py gen-bmgr-config -c ./boards -b esp32_S3_DevKitC_1
```

> `idf.py gen-bmgr-config -c ./boards -b <board_name>` generates the configuration for the specified board. The custom board tree lives in this app's `boards/` directory, which `gen-bmgr-config` does not scan on its own, so `-c ./boards` is required — without it the board is reported as not found. Available board names can be found in the `boards` directory.

2. Configure the device:

Runtime settings are entered through the local provisioning portal and stored in
NVS, so pointing the device at a different Gateway does not need a rebuild:

- Wi-Fi SSID / password
- OpenClaw Gateway WebSocket URL (`ws://` or `wss://`) and optional token
- Device-family label
- Timezone

The portal is reachable at the AP's captive page — the URL and the generated AP
password are printed in the serial log — and, once the device has joined Wi-Fi,
at its station IP. Leave the Gateway URL empty to keep the Native Node disabled.

The device runs no Agent and no LLM, so there are no model keys, IM bot tokens or
search-provider keys to configure: those live on the Gateway.

Compile-time defaults (capability groups, Lua modules, partition table) can be
adjusted through `menuconfig`:

```bash
idf.py menuconfig
```

3. Build and flash:

```bash
idf.py build
idf.py flash monitor
```
