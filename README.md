# ESP-OpenClaw-Node

面向 **M5Stack StickS3 / ESP32-S3** 的 OpenClaw Native Node 固件项目。

本项目把 ESP32 简化为 OpenClaw Native Node：设备只负责 Wi-Fi、身份、硬件命令、Lua 实时编程和显示；Agent、LLM、Memory、IM、权限和通信路由全部放在 Gateway。

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
- 运行时配置 Gateway URL、Token 和设备类型，不需要重新编译固件
- 本地 AP 配网只处理 Wi-Fi 与 OpenClaw 连接，不再提供 LLM/IM 配置
- Lua 脚本、文件沙箱和 LVGL 保留，用于实时编程和快速做显示效果

### Native Node 接口清单

以下命令由固件固定声明，并通过 OpenClaw Gateway 的 `node.invoke` 调用：

#### 设备信息与状态

| 接口 | 参数 | 说明 |
|---|---|---|
| `device.info` | `{}` | 返回设备型号、芯片、固件和能力信息 |
| `device.status` | `{}` | 返回设备运行状态 |
| `device.network` | `{}` | 返回 Wi‑Fi 连接状态、IP 和网关 |
| `device.button.status` | `{}` | 返回按键状态 |

#### 设备控制、显示与音频

| 接口 | 参数 | 说明 |
|---|---|---|
| `device.backlight` | `{"level":0..100}` | 设置背光亮度 |
| `device.restart` | `{}` | 重启设备 |
| `device.screen.clear` | `{}` | 清除通知层并恢复系统主页 |
| `device.screen.text` | `{"text":"..."}` | 在系统 UI 通知区域显示受限中文文本 |
| `device.screen.fullscreen.enter` | `{}` | 进入独立全屏页面 |
| `device.screen.fullscreen.text` | `{"text":"...","orientation":"portrait"}` | 显示全屏中文文本 |
| `device.screen.fullscreen.clear` | `{}` | 清除全屏页面内容 |
| `device.screen.fullscreen.exit` | `{}` | 退出全屏并恢复系统主页 |
| `audio.volume` | 受限音量参数 | 设置音量 |
| `audio.tone` | `{"frequencyHz":880,"durationMs":250}` | 播放提示音 |

全屏文本示例：

```json
{
  "text": "上海今日晴，23–28°C",
  "orientation": "portrait"
}
```

`orientation: "landscape"` 会让全屏文字页按横屏排版（标签对象整体旋转 90°），
但**面板本身仍是竖屏**，LCD 并不会真正旋转。详见「显示接口注意事项」。

#### 受限文件接口

| 接口 | 参数 | 说明 |
|---|---|---|
| `node.files.list` | 路径参数 | 列出沙箱目录 |
| `node.files.read` | 路径参数 | 读取文件 |
| `node.files.write` | 路径和文本内容 | 写入文本文件 |
| `node.files.delete` | 路径参数 | 删除文件 |
| `node.files.copy` | `src_path`、`dst_path` | 复制文件 |
| `node.files.move` | `src_path`、`dst_path` | 移动文件 |

文件接口只允许既有沙箱路径；拒绝 `..` 路径穿越，`/fatfs` 可写，`/system` 只读。

#### 受限 Lua 接口

| 接口 | 说明 |
|---|---|
| `node.lua.run` | 运行设备上已有且通过路径校验的 Lua 脚本 |
| `node.lua.run_async` | 异步运行已有 Lua 脚本 |
| `node.lua.jobs` | 查询异步任务列表 |
| `node.lua.job` | 查询单个异步任务 |
| `node.lua.stop` | 停止指定异步任务 |
| `node.lua.stop_all` | 停止全部异步任务 |

不开放任意 Shell、任意终端、`lua.eval` 或任意命令字符串。

## 显示接口注意事项

### 字体与字符覆盖

屏幕上所有文字都走同一条渲染路径：LVGL `tiny_ttf` + 随固件打包的
`NotoSansSC-Regular-sub.ttf`（约 1.1 MB，3700 左右字形）。系统 UI 的通知区、全屏页、
时钟、Lua `lvgl` 模块的 label，以及 Lua `display` 模块的 `display.text` 系列接口，
用的是同一份字体资源和同一套字号→字形映射，因此同一个字符串在哪条路径上看起来都一致。

`device.screen.text` 与 `device.screen.fullscreen.text` 的 `text` 长度上限是 192 字节；
超长或空字符串会被拒绝。

### 缺字上报

文字是异步入队的，所以 `"shown": true` / `"accepted": true` **只表示命令被接受**，不能区分
「真的画出来了」和「画成一排缺字方框」。为此这两条命令的响应里多了一个字段：

```json
{"command":"device.screen.text","shown":true,"missing_glyphs":0}
{"command":"device.screen.text","shown":true,"missing_glyphs":2,"first_missing":"U+1F9A9"}
{"command":"device.screen.fullscreen.text","accepted":true,"missing_glyphs":null}
```

- `missing_glyphs: 0` —— 所有字符都有字形。
- `missing_glyphs: N` —— N 个字符没有字形，会渲染成空心方框；`first_missing` 给出第一个
  这类字符的码点，方便直接定位。
- `missing_glyphs: null` —— 无法判定。出现在 UI 还没启动（字体未加载）时，或
  `fullscreen.enter/clear/exit` 这类不带文本的命令上。**null 不等于 0**，不要当成通过。

判定用的是通知层实际绘制所用的那份字体，所以结论和屏幕上的结果一致。

### Emoji

`NotoEmoji-Regular-sub.ttf`（约 260 KB，单色 emoji 子集）作为 `lv_font_t.fallback`
挂在 UI 字体后面。`lv_font_get_glyph_dsc()` 会沿这条链查找，所以 emoji 直接按字形渲染，
不需要调用方做任何替换或预处理。固件自身不内置任何 emoji：显示内容全部由 Gateway 下发，
所以某个 emoji 到底画不画得出来，看响应的 `missing_glyphs` 字段，而不是看文档承诺。

如果 emoji 字体文件缺失，文字仍会渲染，emoji 退化为「缺字方框」而不是静默消失。

### 字号

`display.text` 等接口的 `font_size` 是像素高度：

- `0` 表示使用默认值 24。
- 其它取值按需创建并缓存，最多同时缓存 16 个字号；超过 128 会被钳制到 128。

`device.screen.text` 与 `device.screen.fullscreen.text` 不接受字号参数，两者都使用通知字号
（16 px）。

### 换行与方向

- `\n` 换行并按字体行高推进；`\r` 回到行首；`\t` 按 4 个空格宽度推进。
- `device.screen.fullscreen.text` 的 `orientation` 接受 `"portrait"`（默认）与 `"landscape"`，
  大小写不敏感；传 `NULL` 或空串等同于 `"portrait"`。其它取值会记一条 `ESP_LOGW` 并退回竖屏，
  不会静默假装成功。
- 横屏是在**标签对象上做 90° 变换**实现的（`lv_obj_set_style_transform_rotation`），
  而不是旋转显示控制器。面板是固定的 135×240 竖屏 ST7789，SPI 适配器的旋转在注册时就锁定了，
  运行时改 LVGL 逻辑旋转会让 flush 几何错位、全屏页退出后花屏。所以横屏只作用于全屏文字页，
  面板方向本身不变；标签会按长短边互换重新排版后再整体旋转，文字沿长边换行。

### 新增字符

字体是按字符子集打包的，**不在子集里的字符会显示为缺字方框**。新增用户可见文案（C 代码、
Lua 脚本等）后，需要：

1. 跑一遍覆盖审计，看有没有漏掉的字符：

   ```bash
   python3 tools/font_subset/scan_chars.py --check \
       --font application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf \
       --emoji-font application/edge_agent/fatfs_image/system/fonts/NotoEmoji-Regular-sub.ttf
   ```

   它会扫 `components/`、`application/` 和两份 README，把**没有出现在任何字符表里**的字符
   连出处（`文件:行号`）一起列出来。加 `--update` 可以自动补进对应的表。

   这条检查已经挂进 `.pre-commit-config.yaml`（hook id `font-character-coverage`），
   改动 `components/`、`application/`、`tools/font_subset/` 或任一份 README 时会自动运行，
   依赖由 pre-commit 自己装。前提是执行过一次 `pre-commit install`。

2. 重新运行 `tools/font_subset/build_fonts.py` 生成两个字体资源。
3. 重新烧录 `system` 分区。

细节见 `tools/font_subset/README.md`。注意上游 Noto 字体是可变字体，必须先固定
`wght=400` 再取子集，否则桌面端看起来正常、设备端会渲染错误。

只跑 `--check` 不带 `--font` 只能证明**字符表**是完整的，不能证明**字体**里有这些字形 ——
字符可能在表里却仍然没进字体（U+3000 全角空格就踩过这个坑）。要端到端确认，必须带上
`--font`。

## 明确未实现

- 面板级的运行时横竖屏切换（全屏文字页的横屏是通过对象变换实现的，不改变面板方向本身）
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

## 配网与安全边界

- Gateway URL、Token 和设备类型通过本地 AP 配网门户保存到 NVS，源码中不再放置连接凭据；URL 留空时 Native Node 不启动。
- 配网门户只负责 Wi-Fi 与 OpenClaw 连接，不提供 LLM、Memory、IM 或搜索服务配置。
- Gateway Token、Wi-Fi 密码和连接字符串不得出现在日志、文档或提交中。
- **`GET /api/config` 不回读密钥。** `wifi_password`、`ap_password` 和
  `openclaw_gateway_token` 已设置时返回固定掩码 `********`；前端原样回传即表示"保持不变"，
  传空串则清除该项。真实值只存在于设备 NVS，不会经 HTTP 离开设备。
- **配网 AP 首次启动自动设密码。** 门户本身没有鉴权，同网段任何人都能读写配置、把 Gateway URL
  改指向其它服务器，所以首次启动会生成 12 位随机 AP 密码、持久化到 NVS 并打印在串口日志里，
  使配网 AP 为 WPA2 而非开放。在门户里清空 `ap_password` 仍可恢复开放 AP（供刻意需要时使用）。
- 生产部署仍建议给配网门户加鉴权：当前做到了「不泄露密钥」，但**未**做到「阻止改配置」。
- 设备身份种子持久化在 NVS；当前开发 `sdkconfig` 没有开启 Flash 加密。生产设备必须启用 ESP Flash/NVS 加密或使用安全元件，才能把该身份视为硬件信任根。**不要在已有设备上直接开启加密并烧录，先备份、确认密钥/量产流程和恢复方案。**
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
