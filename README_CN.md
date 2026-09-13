# ESP-OpenClaw-Node

面向 **M5Stack StickS3 / ESP32-S3** 的 OpenClaw Native Node 固件项目。

本项目将 OpenClaw Gateway 的 Agent、LLM、Memory、权限和通信路由与 ESP32 设备执行层分离。ESP32 只执行固件中明确声明、校验和限制的设备命令，不运行 Agent 或 LLM。

## 当前状态

- Native Node WebSocket、`connect.challenge`、`hello-ok` 和 v3 Ed25519 Device Auth：已在真实 M5Stack StickS3 验证
- NVS 持久化设备身份和稳定 Node ID：已验证
- Gateway 配对、命令面批准和 `node.invoke`：已验证
- 设备信息、状态、网络、按键、背光、重启、音频命令：已验证
- 受限文件和 Lua 脚本命令：已验证
- LVGL + Tiny TTF + NotoSansSC 中文显示：已验证
- 中文自动换行和受限天气符号图形替换：已实现，最终视觉效果仍需实体照片确认
- 稳定竖屏显示：已验证
- 真正运行时横屏：未完成，当前不得宣称支持

## 显示命令

```text
device.screen.text
device.screen.clear
device.screen.fullscreen.enter
device.screen.fullscreen.text
device.screen.fullscreen.clear
device.screen.fullscreen.exit
```

`accepted`、`shown` 和 `cleared` 仅代表命令被接受，不代表屏幕视觉成功。必须以真实设备照片或视频确认文字边界、中文字体、图形替换、全屏退出和是否花屏。

## 架构

```text
OpenClaw Gateway
  ├─ Agent / LLM / Memory / authorization / routing
  └─ WebSocket invokes Native Node
        ↓ Wi-Fi
M5Stack StickS3 / ESP32-S3
  └─ identity, display, audio, and bounded hardware commands
```

## 安全

默认固件保持空连接配置：

```c
#define OPENCLAW_NODE_GATEWAY_URL ""
#define OPENCLAW_NODE_GATEWAY_TOKEN ""
```

Gateway Token、Wi-Fi 密码、API key、私钥和连接字符串不得提交 Git 或写入文档、日志。联调结束后恢复空 URL/Token，并将 Gateway 恢复为 `bind=loopback`。不开放任意 Shell、任意终端、`lua.eval` 或任意命令字符串。

## 构建

已验证 ESP-IDF 5.5.4 / Python 3.9.6：

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

烧录前必须核对 M5Stack StickS3、ESP32-S3-PICO-1、8 MB Flash、8 MB PSRAM 和目标串口。

## 项目名称与图形

项目名称统一为 **ESP-OpenClaw-Node**。仓库首页不再使用原 ESP-Claw 的 Logo、龙虾图形、宣传图或品牌链接；仓库内保留的上游实现文件只属于代码基础或第三方依赖，不代表本项目产品标识。

## 许可证

见 [`LICENSE`](./LICENSE)。
