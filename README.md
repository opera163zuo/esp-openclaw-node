<div align="center">

  <a href="https://github.com/opera163zuo/esp-openclaw">
    <img src="./docs/src/assets/logos/logo.svg" alt="ESP-OpenClaw logo" width="45%" />
  </a>

  <h1>ESP-OpenClaw 🦞</h1>
  <h3>面向 ESP32 的 OpenClaw Native Node 与边缘 Agent 项目</h3>

  <p>
    <a href="https://github.com/opera163zuo/esp-openclaw">
      <img src="https://img.shields.io/badge/platform-ESP32--S3-blue?style=flat-square" alt="ESP32-S3" />
    </a>
    <a href="./LICENSE">
      <img src="https://img.shields.io/github/license/opera163zuo/esp-openclaw?style=flat-square" alt="License" />
    </a>
  </p>

  <a href="./README_EN.md">English</a>
  |
  <a href="https://esp-claw.com/">ESP-Claw 文档</a>
  |
  <a href="./application/edge_agent/README.md">构建说明</a>

</div>

## 项目简介

ESP-OpenClaw 是基于 Espressif ESP-Claw 改造的 OpenClaw Native Node 项目，当前重点适配 **M5Stack StickS3**。

项目采用明确的分层架构：

```text
OpenClaw Gateway
  ├─ Agent / LLM / Memory / 权限 / 通信路由
  └─ 通过 WebSocket 调用 Native Node
        ↓ Wi‑Fi
M5Stack StickS3 / ESP32-S3
  └─ 设备身份、硬件能力和固定命令执行
```

ESP32 不运行 OpenClaw Agent、LLM 或社交平台逻辑。复杂理解、任务编排、权限控制和通信路由由 Gateway 负责；设备只执行经过声明和限制的命令。

> 本仓库仍保留 ESP-Claw 原有的边缘 Agent、Capability、Lua、Memory 和配置系统。Native Node 是本项目新增并已在 M5Stack StickS3 上真实验收的主要方向。

## 当前状态

### 已完成并在真实设备上验证

- OpenClaw Native Node WebSocket 连接与协议 v4 握手
- `connect.challenge` 与 v3 Ed25519 Device Auth
- NVS 持久化设备身份
- 基于原始 Ed25519 公钥 SHA-256 的稳定 Node ID
- Gateway 配对、命令面批准和 `node.invoke` 调用
- M5Stack StickS3：ESP32-S3-PICO-1、8 MB Flash、8 MB PSRAM
- 设备信息与状态：
  - `device.info`
  - `device.status`
  - `device.network`
  - `device.button.status`
- 显示与声音：
  - `device.backlight`
  - `device.screen.clear`
  - `audio.volume`
  - `audio.tone`
- 设备控制：
  - `device.restart`
- 受限文件能力：
  - `node.files.list`
  - `node.files.read`
  - `node.files.write`
  - `node.files.delete`
  - `node.files.copy`
  - `node.files.move`
- 受限 Lua 脚本与任务：
  - `node.lua.run`
  - `node.lua.run_async`
  - `node.lua.jobs`
  - `node.lua.job`
  - `node.lua.stop`
  - `node.lua.stop_all`

以上命令已通过真实 M5StickS3 与 OpenClaw Gateway 的端到端调用验证。文件测试使用临时文件并已清理；Lua 测试使用设备内已有脚本，不是任意 Lua 字符串执行。

### 当前未完成

- BLE HID Native Node 命令、电脑端蓝牙配对及键盘/鼠标实际输入验收
- `cap_cli` 的 Native Node 受限接入
- 摄像头、截图、屏幕读取和完整电脑控制
- 任意 Shell、任意终端、任意桌面脚本
- Native Node 录音、音频流、STT/TTS 和连续语音对话
- OTA 固件升级、远程刷写和生产级密钥轮换
- 更完整的屏幕文字/UI Native Node API

当前不能把 BLE HID、截图、远程桌面或完整电脑控制称为已实现功能。

## Native Node 命令参数示例

### 播放提示音

```json
{
  "frequencyHz": 880,
  "durationMs": 250
}
```

限制：频率 `100–4000 Hz`，时长 `1–2000 ms`。

### 复制和移动文件

```json
{
  "src_path": "/fatfs/source.txt",
  "dst_path": "/fatfs/copy.txt"
}
```

复制和移动使用 `src_path` / `dst_path`，不是 `source` / `destination`。

### 执行设备内已有 Lua 脚本

```json
{
  "path": "/system/skills/builtin_lua_modules/scripts/builtin/test/system_info.lua",
  "timeout_ms": 5000
}
```

异步任务示例：

```json
{
  "path": "/system/skills/builtin_lua_modules/scripts/builtin/test/system_info.lua",
  "timeout_ms": 5000,
  "name": "protocol-test",
  "log_bytes": 2048
}
```

## 安全边界

- 文件命令只接受绝对路径。
- 禁止包含 `..` 的路径穿越。
- `/fatfs` 是可写数据区；`/system` 是只读固件区。
- 文件能力保留原有沙箱和大小限制。
- Lua 只能运行设备上已有并通过路径校验的脚本。
- 不开放 `shell.exec`、`terminal.exec`、`lua.eval` 或任意命令字符串。
- 不把 Native Node 调用结果当成完整电脑控制结果。
- Gateway URL、Token、Wi‑Fi 密码、私钥和其他凭据不得提交 Git。

## 硬件与构建

已验证环境：

```text
Board: M5Stack StickS3
MCU: ESP32-S3-PICO-1
Flash: 8 MB
PSRAM: 8 MB
ESP-IDF: 5.5.4
Serial: /dev/cu.usbmodem101（示例）
```

准备 ESP-IDF 后构建：

```bash
cd application/edge_agent
source "$HOME/esp/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

烧录前请确认目标板、芯片型号、Flash、PSRAM 和串口。烧录：

```bash
idf.py -p /dev/cu.usbmodem101 flash
```

Native Node 默认关闭连接配置。生产或安全固件应保持：

```c
#define OPENCLAW_NODE_GATEWAY_URL ""
#define OPENCLAW_NODE_GATEWAY_TOKEN ""
```

真实 Gateway 联调需要临时使用本地未提交配置，完成后恢复空 URL/Token，并将 Gateway 恢复为 loopback 监听。

## 代码结构

```text
components/openclaw_node/                 Native Node 传输、身份和命令分发
components/claw_capabilities/cap_files/   受限文件 Capability
components/claw_capabilities/cap_lua/     Lua 脚本与异步任务 Capability
components/claw_capabilities/cap_cli/     原有受限 ESP Console Capability
application/edge_agent/                   M5Stack StickS3 应用与板级集成
components/lua_modules/                   原有 Lua 模块
```

## GitHub 修改记录

本项目的 Native Node 主要修改已提交到 `master`，包括：

- Native Node WebSocket 传输和 OpenClaw 握手
- Monocypher Ed25519 身份与 NVS 持久化
- 设备信息、状态、网络、按键、背光、重启、音频和屏幕命令
- 受限文件和 Lua Capability 的 Native Node 映射
- ESP-IDF Component Manifest 与依赖修复
- 真实 M5StickS3 / Gateway 联调修复

临时 Gateway URL、Token 和 Wi‑Fi 凭据不在 GitHub 中。

## 上游项目与致谢

本项目基于 [Espressif ESP-Claw](https://github.com/espressif/esp-claw)；感谢其 Agent Loop、Capability、Lua、Memory、板级支持和文档工作。

Native Node 协议方向受到 [OpenClaw](https://github.com/openclaw/openclaw) 启发。

## 许可证

请参阅 [`LICENSE`](./LICENSE)。
