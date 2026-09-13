# ESP-OpenClaw-Node

面向 **M5Stack StickS3 / ESP32-S3** 的 OpenClaw Native Node 固件项目。

本项目将 OpenClaw Gateway 的 Agent、LLM、Memory、权限和通信路由与 ESP32 设备执行层分离：

```text
OpenClaw Gateway
  ├─ Agent / LLM / Memory / 权限 / 路由
  └─ WebSocket 调用 Native Node
        ↓ Wi‑Fi
M5Stack StickS3 / ESP32-S3
  └─ 设备身份、显示、音频和受限硬件命令
```

ESP32 不运行 Agent 或 LLM，只执行固件中明确声明、校验和限制的设备命令。

## 项目名称

- 项目名称：**ESP-OpenClaw-Node**
- 当前硬件：M5Stack StickS3
- 芯片：ESP32-S3-PICO-1
- 屏幕：ST7789 SPI，135×240，RGB565
- Flash / PSRAM：8 MB / 8 MB
- 协议：OpenClaw Native Node protocol v4，设备认证使用 v3 Ed25519 Device Auth

## 当前已实现

### Native Node

- WebSocket 连接、`connect.challenge` 和 `hello-ok`
- Monocypher Ed25519 身份
- NVS 持久化设备身份
- 基于原始 Ed25519 公钥 SHA-256 的稳定 Node ID
- Gateway 配对、命令面批准和 `node.invoke`
- `device.info`、`device.status`、`device.network`
- `device.button.status`、`device.backlight`、`device.restart`
- `audio.volume`、`audio.tone`

### 显示

- 复用原工程的 LVGL + Tiny TTF + NotoSansSC 中文字体链路
- `device.screen.text` 中文通知显示
- `device.screen.fullscreen.enter`
- `device.screen.fullscreen.text`
- `device.screen.fullscreen.clear`
- `device.screen.fullscreen.exit`
- 中文文本自动换行
- 对天气和常用符号提供受限图形替换，避免字体缺字显示方框
- 所有 LVGL 对象操作通过 System UI 线程执行

当前稳定显示方向是**竖屏**。`orientation: landscape` 尚未实现真正的 LCD 横屏切换，不能把命令接受或 API 返回当作横屏完成。真正横屏需要同步重建 ST7789 panel、LVGL adapter、frame buffer、flush 坐标和 UI 页面。

### 受限文件与 Lua

- `node.files.list/read/write/delete/copy/move`
- `node.lua.run/run_async/jobs/job/stop/stop_all`
- 文件路径限制在既有沙箱内；拒绝 `..` 路径穿越
- `/fatfs` 可写，`/system` 只读
- Lua 只能运行设备上已有且通过路径校验的脚本

## 明确未实现

- 稳定的运行时横屏/竖屏切换
- BLE HID 的真实配对及主机键盘/鼠标输入验收
- 摄像头、截图、屏幕读取和完整电脑控制
- 任意 Shell、任意终端和任意桌面脚本
- Native Node 录音、音频流、STT/TTS 和连续语音对话
- OTA 固件升级、远程刷写和生产级密钥轮换

## 构建与烧录

已验证环境：ESP-IDF 5.5.4、Python 3.9.6。

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

烧录前必须再次确认目标板、芯片、Flash、PSRAM 和串口。成功烧录不等于功能验收；必须另外确认启动日志、Gateway 连接、命令面批准、实际调用和实体屏幕效果。

## 安全边界

- 默认固件不包含 Gateway URL 和 Token：

```c
#define OPENCLAW_NODE_GATEWAY_URL ""
#define OPENCLAW_NODE_GATEWAY_TOKEN ""
```

- 联调凭据只允许使用本机临时覆盖，不得提交 Git。
- Gateway Token、Wi‑Fi 密码、API key、私钥和连接字符串不得出现在日志、文档或提交中。
- 联调结束后恢复空 URL/Token，并将 Gateway 恢复为 `bind=loopback`。
- 不开放 `shell.exec`、`terminal.exec`、`lua.eval` 或任意命令字符串。

## 目录

```text
components/openclaw_node/                 Native Node 传输、身份和命令分发
components/common/system_ui/               LVGL System UI 与中文显示
components/common/display_service/         LCD、LVGL adapter 和显示生命周期
components/claw_capabilities/cap_files/   受限文件能力
components/claw_capabilities/cap_lua/     Lua 脚本与任务能力
application/edge_agent/                   ESP-IDF 应用和 M5Stack StickS3 集成
```

## 验收原则

`accepted: true`、`shown: true` 和 `cleared: true` 只代表命令被固件接受，不代表实体屏幕已经正确显示。显示功能必须用真实设备照片或视频确认，并分别记录 API、烧录、连接、命令调用和视觉结果。

## 上游说明

本项目源自 Espressif 的 ESP-Claw 代码基础，但当前仓库产品名称、GitHub 说明和 Native Node 方向统一为 **ESP-OpenClaw-Node**。原项目的品牌 Logo 和宣传图不作为本项目的产品标识。

## 许可证

见 [`LICENSE`](./LICENSE)。
