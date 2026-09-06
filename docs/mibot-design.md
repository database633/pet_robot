# Mibot 项目设计文档（现行全貌）

- 日期：2026-09-06
- 状态：M1 已实现并通过全部评审（READY_WITH_NOTES，硬化收口中）；M2+ 为设计态
- 本文档是整个项目的单一入口视图：已实现部分给到字节级细节，设计态部分给到模块级摘要
- 详细文档索引见 §13

---

## 1. 项目定位与三平面架构

Mibot 是桌面机器人。按《Mibot 桌面机器人状态机与控制接口规范》（下称《规范》）§1 的三平面划分：

| 平面 | 运行位置 | 职责 | 本仓库交付物 |
|---|---|---|---|
| 交互与 Agent | **SF32LB53 / openvela** | Agent loop、对话上下文、Skill、唤醒/VAD、LCD 表情 | ❌ 不在本仓库（契约见 §9；实现蓝图见 spec v2 §11） |
| 实时设备 | **ESP32-S3（本仓库）** | UART 协议栈、AI 网关、运动安全执行器、驱动 | ✅ 本仓库全部新代码 |
| 云端 AI | LLM / ASR / TTS / 视觉 | 推理服务 | 不交付；本仓库是其调用方 |

基线仓库：78/xiaozhi-esp32（分支 `feat/device-agent`）。全部新代码集中于 `main/boards/mibot/`，对上游共享文件仅 3 处注册性修改（Kconfig + CMakeLists 共 5 行）——板卡机制天然隔离上游演进冲突。

## 2. 权威关系与安全模型（《规范》§1.1）

- **动作意图权威 = SF32 Agent**；**执行与安全权威 = ESP32-S3**（可拒绝任何越界/过期/重复命令）；**最终停止权威 = ESP32-S3 本地安全任务**（不依赖 UART、不依赖云端）。
- 云端 API Key 只存 ESP32 NVS，**不经 UART 传递**（《规范》§7）。
- "生成 Tool Call ≠ 已执行"：ESP32 的 ACK `state:completed` 信封是唯一执行凭证。
- 断云降级：`cloud_disconnected` 期间仅保留本地停止/唤醒（《规范》§5.2）；完成定义 = **断云状态下**仍能本地停止、识别 ToF 异常、拒绝危险动作。

## 3. 硬件前提

- ESP32-S3，PSRAM ≥2MB
- UART 连接 SF32：**921600 8N1，3.3V TTL**，独立硬件 UART（不与 console 复用）；引脚为占位默认值（TX=GPIO17/RX=GPIO18，以原理图为准，见 `mibot_config.h`）
- 4 路 ToF（I²C，TCA9548A 分址或 XSHUT：front_left/front_right/rear_left/rear_right）；2×直流电机（TB6612）+ 2×舵机；相机（JPEG）；电池电压检测
- **板卡无本地麦克风/喇叭**：音频全部经 UART 与 SF32 往返（xiaozhi AudioService 以 DummyAudioCodec 兼容初始化）

## 4. 里程碑路线图与当前状态

| 里程碑 | 内容 | DoD（《规范》§8.2） | 状态 |
|---|---|---|---|
| **M1** | UART 链路全栈 + 板卡骨架 + console 诊断 + PC 对端 | 步骤 1：链路稳定 1h 无 CRC 错误堆积 | ✅ 代码完成，评审 READY；设备端验证（Task 10）待硬件 |
| M2 | ToF 阵列 + 边缘检测 + BRAKING | 步骤 2（含拔 UART 本地刹车） | 设计态 |
| M3 | command_exec + 电机/舵机驱动 | 步骤 3 | 设计态 |
| M4 | ai_gateway（ASR/LLM）+ audio_relay 上行 | 步骤 4 前半（ASR 闭环） | 设计态 |
| M5 | tts_client + AUDIO_DOWN + speak | 步骤 4 后半 + 步骤 5 | 设计态 |
| M6 | camera_capture + vision_client | 步骤 6 | 设计态 |
| M7 | BLE 配网、主动任务、多轮规划 | 后续版本 | 未排期 |

## 5. UART 链路协议（M1，已实现 + 交叉验证）

### 5.1 帧格式（《规范》§3.2，字节级）

```
偏移  字段      长度  说明
0     SOF       2     0xAA 0x55
2     VERSION   1     0x01
3     TYPE      1     消息类型（§5.2）
4     FLAGS     1     bit0 请求应答 / bit1 应答 / bit2 错误 / bit3 分片
5     SEQ       2     小端，回绕安全
7     LENGTH    2     小端，载荷上限 4096（解码端强制）
9     PAYLOAD   n     n = LENGTH，最大 4096
9+n   CRC       2     CRC-16/CCITT-FALSE 小端存放，覆盖 VERSION..PAYLOAD
```

- 线帧总长 = 11 + n 字节（最大 4107）
- CRC 参数：poly 0x1021，init 0xFFFF，无反射无异或输出；校验向量 `"123456789"` → `0x29B1`
- 实现注意：HELLO_ACK 的 `max_payload` 字段指**载荷**上限（=LENGTH 字段上限 4096），SF32 接收缓冲应按 `max_payload + 11` 分配

### 5.2 消息类型与实现状态

| TYPE | 名称 | 方向 | M1 状态 |
|---|---|---|---|
| 0x01 | HELLO | SF32→ESP32 | ✅ 处理（解析 fw_version，回 HELLO_ACK，置 ready） |
| 0x02 | HELLO_ACK | ESP32→SF32 | ✅ 发送（JSON 见 §6.1） |
| 0x10 | COMMAND | SF32→ESP32 | M3（现回 NACK E_UNSUPPORTED） |
| 0x11 | ACK | ESP32→SF32 | M3 |
| 0x12 | NACK | ESP32→SF32 | ✅ 发送（E_UNSUPPORTED，帧类型真 hex 渲染） |
| 0x20 | EVENT | ESP32→SF32 | M2（边缘检测/异常上报） |
| 0x21 | TELEMETRY | ESP32→SF32 | ✅ 1Hz 发送（schema 见 §6.3） |
| 0x30 | AUDIO_UP | SF32→ESP32 | M4 |
| 0x31 | AUDIO_DOWN | ESP32→SF32 | M5 |
| 0x40 | AI_REQUEST | SF32→ESP32 | M4 |
| 0x41 | AI_RESPONSE | ESP32→SF32 | M4 |
| 0x50 | CAMERA_RESULT | ESP32→SF32 | M6 |
| 0x60 | PING | SF32→ESP32 | ✅ 处理 |
| 0x61 | PONG | ESP32→SF32 | ✅ 发送（seq 回写，payload `{}`） |

FLAGS：`0x01` 请求应答（kAckRequest）、`0x02` 应答（kAck）、`0x04` 错误（kError）、`0x08` 分片（kFragment）。

### 5.3 分片（FLAGS bit3）

- 分片帧 payload = `[total(1B), index(1B)] + data`；同 TYPE + 同 SEQ；按序到达；index 0 起始
- 重组规则（`FragmentReassembler`）：链必须从 index=0 开始；相邻分片静默 **>500ms** / SEQ 或 TYPE 变化 / index 不连续 → 丢弃旧链
- 重组总量上限 **64KB**（增长守卫强制，超出弃链）；覆盖 M4 的长 AI_REQUEST
- 非分片帧经同一入口直通（RX 单一分发路径）

### 5.4 错误观测

`FrameDecoder::error_count()` 生命周期级累计（版本错/长度超限/CRC 错各计一次；Reset 保留）。设备侧两路暴露：RxLoop 检测到计数变化即 `ESP_LOGW`；`mibot show` 输出 `errors=N`。§8.2 步骤 1 的"1h 无 CRC 错误堆积"判据由此观测。

### 5.5 解码器已知限制（登记 M2）

完整假 SOF（AA 55 + 恰能通过版本/长度校验的假头）会吞掉后续真实帧字节且不计数；丢弃后从下一 SOF 重同步。噪声流下 C++ 端与 Python 对端可能丢不同的帧——M2 补对等重扫（丢弃假头时回退重扫）。

## 6. M1 会话层契约（JSON）

统一 `schema: "mibot.uart.v1"`。以下为当前固件（v0.1.0）的精确行为：

### 6.1 HELLO → HELLO_ACK

收 HELLO（解析 `fw_version` 用于日志；schema/proto_version 校验未做——M2）后回：

```json
{"schema":"mibot.uart.v1","proto_version":1,"max_payload":4096,"fw_version":"0.1.0","capabilities":["telemetry"]}
```

`capabilities` 为**本固件实际能力**：M1 仅 telemetry；audio_relay/motion/ai_gateway/camera 随 M2-M6 逐项加入，SF32 不应按未宣告能力发起 COMMAND（现一律 NACK）。

### 6.2 PING → PONG

seq 回写 = PING 的 seq（对端据此测 RTT），payload = `{}`，FLAGS=kAck。

### 6.3 TELEMETRY（1Hz，TELEMETRY 任务独立于会话）

```json
{"schema":"mibot.telemetry.v1","ts_ms":123456.0,"behavior_state":"-","motion_state":"MOTION_INIT",
 "battery_mv":0,"tof":null,"motor":{"left":"off","right":"off"},"servo":null,
 "wifi":{"connected":true,"rssi_dbm":-52}}
```

占位语义：`behavior_state` 属 SF32（ESP32 未知，恒 "-"）；`motion_state` M2 接状态机；`battery_mv` M3 接 ADC；`tof`/`servo` M2 接驱动前为 null。`ts_ms` 为 ESP32 开机毫秒（int64 来源，double 承载，无 32 位回绕）。

### 6.4 NACK（未支持帧类型）

```json
{"schema":"mibot.uart.v1","error":{"code":"E_UNSUPPORTED","message":"type 0x10 not supported in fw 0.1.0"}}
```

FLAGS=kError，seq 回写。错误码全集（《规范》§5.3）在 M3 command_exec 逐条落地：`E_DUPLICATE / E_EXPIRED / E_INVALID_ARG / E_SAFETY_LOCK / E_BUSY / E_LOW_BATTERY / E_TOF_INVALID / E_PAYLOAD_TOO_LARGE / E_CLOUD_TIMEOUT / E_PROTOCOL_CRC / E_UNSUPPORTED / E_SERVO_POWER`。

## 7. ESP32-S3 固件模块设计（现行代码）

### 7.1 文件清单与职责（`main/boards/mibot/`）

| 文件 | 职责 |
|---|---|
| `mibot_frame_codec.{h,cc}` | 帧编解码 + 分片重组。**纯 C++，禁止 ESP-IDF 头**（宿主单测直接编译本文件） |
| `mibot_uart_link.{h,cc}` | RX/TX 任务、队列、Send 拷贝语义、错误计数暴露 |
| `mibot_link_service.{h,cc}` | 会话层：HELLO/PING/TELEMETRY/NACK（全局命名空间类，供 console 与 board 共用） |
| `mibot_console.{h,cc}` | esp_console REPL 诊断（`mibot show`），非阻塞启动 |
| `mibot_board.cc` | 板卡注册（对接 xiaozhi Board 框架），持有链路与服务的生命周期 |
| `mibot_config.h` | UART/音频/版本常量（引脚为占位） |
| `config.json` | 板卡变体声明（type/name = `mibot-esp32s3`，target esp32s3） |

### 7.2 线程模型

| 任务 | 栈 | 优先级 | 职责 |
|---|---|---|---|
| `mibot_rx` | 6144 | 5 | 20ms 轮询 `uart_read_bytes` → 解码 → 分片重组 → handler 回调（cJSON 在此任务执行） |
| `mibot_tx` | 4096 | 5 | 队列取 `Frame*` → 编码 → 部分写循环（5ms 退避） |
| `mibot_tel` | 4096 | 4 | 1Hz 遥测，`vTaskDelayUntil` 不漂移（专用任务避免 esp_timer 共享小栈） |
| `mibot_cli` | 6144 | 3 | esp_console REPL（UART0 115200 GPIO43/44，prompt `mibot> `） |

- TX 队列 16 × `Frame*`；`Send()` 拷贝入队，**队满直接丢弃**（重传是上层职责；M2 引入 50Hz 音频后改内存池/双队列）
- `MibotLinkService` 不提供 Stop（与板卡同生命周期）；M2 按需加 `running_` + 任务自退出
- 任务创建统一 `pdPASS` 判断后转 `ESP_ERROR_CHECK`（xTaskCreate 成功返回 1 ≠ ESP_OK 0，直包会在成功时 abort）

### 7.3 板卡注册（四方一致）

`config.json` type/name `mibot-esp32s3` ⇔ Kconfig `BOARD_TYPE_MIBOT_ESP32S3`（depends IDF_TARGET_ESP32S3）⇔ CMakeLists `BOARD_DIR "mibot"` ⇔ `GetBoardType()` 返回 `"mibot-esp32s3"`。板卡类 `MibotBoard : WifiBoard`，`DummyAudioCodec` 兼容初始化，构造即启动链路服务与 console。新 `.cc` 由板卡 GLOB 自动纳入编译，无需改 CMake。

### 7.4 console 诊断

```
mibot show    → fw=0.1.0 uart=1 baud=921600 tx=17 rx=18 ready=yes errors=0
```

（M4 起按 spec v2 §4.2 扩展 `mibot set llm.key sk-xxx` 等 NVS 配置命令。）

## 8. 后续里程碑模块设计（M2-M6，设计态）

摘要如下；完整设计见 spec v2 §4。

### 8.1 motion——运动安全执行器（M2/M3，硬实时核心）

**状态机**（《规范》§2.2）：`MOTION_INIT → STANDBY ⇄ RUNNING`；`BRAKING`（边缘/急停/租约失效/停止命令 → 立即断 PWM）；`HOLD`（BRAKING 后待确认）；`FAULT`（ToF 全失效/堵转 → 禁止一切运动）。

**ToF 边缘检测**：独立任务 20Hz 轮询四路；默认参数 `edge_threshold_mm=50`、`edge_confirm_count=3`、`motion_slowdown_mm=120`、`sensor_timeout_ms=250`。**任意前方传感器连续 3 次超阈值 → 不经 UART、不等云端，本地直接 BRAKING** 并发 EVENT。拔掉 UART 仍生效（M2 DoD）。

**command_exec 校验链**（M3，COMMAND 0x10 统一入口，逐级拒绝）：幂等（`command_id` 缓存 → E_DUPLICATE）→ TTL（E_EXPIRED）→ 参数限幅（linear ±120mm/s、angular ±90°/s、duration 50-5000ms 默认 500、舵机 0-180 按机械限位收紧 → E_INVALID_ARG）→ 安全状态（E_SAFETY_LOCK/E_BUSY/E_LOW_BATTERY/E_TOF_INVALID）→ 执行 → ACK/NACK 信封（《规范》§4.1：`{ok, command_id, state, result, error}`）。`avoid_edge` 永远开启。

**保活租约**：运动期间 1500ms 无有效保活/租约刷新 → 本地直接 BRAKING（《规范》§3.5）。

### 8.2 ai_gateway（M4-M5）

ESP32 持有全部云端配置与 TLS（NVS 命名空间 `mibot`：llm/asr/tts/vision 各 url/key/model）。`AI_REQUEST` 不解释 `request` 内容，仅注入鉴权/URL/model 后 HTTPS 转发；非流式、同时刻仅一条云端连接、响应 >8KB 中止；payload 分片重组上限 64KB。超时 30s（LLM）/10s（ASR），各重试 1 次。`AI_RESPONSE` 事件种类：`asr_text / assistant（content+tool_calls）/ error / done`。

### 8.3 audio_relay / camera（M4-M6）

上行：首帧元数据（pcm_s16le/16000/mono/20ms/640B/eos）+ 纯 PCM，累积 ≤30s；下行：TTS PCM 重采样 16k 后 20ms 分帧流式下发。相机按需拍照 → JPEG 上云 → CAMERA_RESULT 回传（完整 JPEG 不经 UART）。

## 9. SF32 侧（openvela）与云端 AI Tool 契约

- SF32 侧实现蓝图（spec v2 §11）：AgentLoop/AgentContext/ToolRegistry（pi agent-core 移植）、`LlmTransport` 的 UART AI_REQUEST 客户端实现、远程 MCP 工具与 Skill 扩展点。宿主 mock 测试方案原样适用。
- 云端 AI Tool 契约（《规范》第 4 章逐字采用）：`robot.get_status / move / rotate / stop / set_arm_pose / set_expression / speak / capture_image / read_floor_sensors`。`set_expression` 属 SF32 本地 LCD Skill，误发 ESP32 回 NACK E_UNSUPPORTED。统一返回信封与 E_* 错误码双端一致。

## 10. 验证设施（本机无 ESP-IDF/cmake；zig 为宿主编译器）

### 10.1 宿主单元测试（`test/host/`，18 项全绿）

```bash
"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ \
  -std=c++17 -Wall -Wextra -Imain/boards/mibot -Itest/host \
  test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc \
  -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe
```

覆盖：CRC 校验向量/空输入/分段等价、编码布局/往返/双帧同 feed、垃圾重同步、坏 CRC 丢弃、超长丢弃、空载荷、逐字节 feed、分片重组/失活超时/长链/单分片/超 64KB 弃链。零警告。

### 10.2 Python 对端（`scripts/mibot_uart_peer.py`，SF32 模拟器）

```
python scripts/mibot_uart_peer.py COM5 hello          # HELLO→HELLO_ACK
python scripts/mibot_uart_peer.py COM5 ping 10        # N 次 PING，RTT 统计
python scripts/mibot_uart_peer.py COM5 telemetry 20   # 观察 N 秒遥测
python scripts/mibot_uart_peer.py COM5 frag 512       # 分片 PING→PONG（验证设备重组）
```

依赖 pyserial（3.5）。Decoder 1 字节滑动重同步；`encode_fragments` 按 §5.3 线格式切分（`[total,index]+data`，kFragment，同 SEQ）。

### 10.3 交叉验证结论（评审产出）

- **C++ ↔ Python 字节级对等：双向 7/7 全同**（hello/hello_ack/pong/ping/telemetry/nack/4096B 最大载荷 + seq=65535 回绕），CRC 区域、字节序、4096 上限两侧一致
- 宿主 18/18 零警告；板卡注册四方一致且 `scripts/build.py --list-boards` 可见
- 评审者以一次性 harness 实测 Python 分片字节流 → C++ FragmentReassembler 重组逐字节相等

## 11. 已知限制与 M2 遗留登记（全部不阻塞 M1 闭环）

**链路与协议**：
- 无链路 Stop/重启（与板卡同生命周期）；COMMAND 执行必须投递独立任务（不得阻塞 RX）
- `Send()` 每帧一次堆分配 → M2 内存池/双队列（50Hz 音频前必须）
- SEQ 单调性检查未实现；`FragmentReassembler::Feed` 裸 bool 无法区分超时/换链/乱序/超限（E_PAYLOAD_TOO_LARGE 应答需要枚举返回）
- 分片帧 `assembled.flags=0` 丢弃 kAckRequest——写分片发送端（SF32 侧）前必须决策并文档化
- 首分片 `reserve` 可被恶意 total=255 触发一次 64KB 连续分配（`-fno-exceptions` 下 abort 向量）→ M2 移除预留
- 假 SOF 不回退重扫（§5.5）；HELLO 未校验 schema/proto_version 即 ACK
- 遥测未按 ready_ 门控（上电即发）；TxLoop 持久写失败无限重试会堵死 TX 队列
- console 常量硬编码于 mibot_console.cc；对端波特率硬编码 921600（建议加 --baud）；命名空间风格不统一（mibot:: 与全局类并存）；cJSON 打印/发送 6 行序列重复

**设备端验证缺口（Task 10 首日即可命中）**：
- 设备侧从未编译（本机无 IDF）——`esp_console_dev_uart_config_t` 字段名、3 个 cJSON Add*ToObject API、`uart_read_bytes` 等为 canonical 但无仓库先例，首次 `idf.py build` 是修正点
- HELLO_ACK/PONG/NACK payload 的 JSON 形状无宿主测试钉住（link_service 依赖 IDF），对端断言是第一道网
- 1Hz 遥测节拍、TX 队满丢弃路径、真实噪声下的重同步对等——均待设备端验证

**首次配网入口缺失**：M1 无按键无屏幕，NVS 无凭据时无法配网——验证用预烧录凭据；M2 必须补配网通道（UART 命令触发或保留 BOOT 键）。

## 12. 资源与时序预算（spec v2 §5/§6，M2+ 生效）

- 内部 RAM ~45KB 峰值（单 TLS 连接串行）；PSRAM：上行 PCM ≤960KB、TTS 缓冲 ≤128KB、LLM ≤64KB/8KB、JPEG ≤512KB；UART DMA 环形 RX/TX 各 16KB
- 任务预算（M2+）：motion 4KB 最高优先级（BRAKING 路径零动态分配）、ai_gateway 8KB、tof 3KB
- 时序：ASR 1-2s、LLM 1-3s/轮、控制帧 <10ms、TTS 首音 1-2s；loop 迭代由 SF32 限定（建议 ≤8）

## 13. 文档与仓库索引

| 文档 | 路径 | 性质 |
|---|---|---|
| 《Mibot 桌面机器人状态机与控制接口规范》 | `D:\下载\Mibot 桌面机器人状态机与控制接口规范.md` | 用户权威规范（协议/工具/验收源头） |
| ESP32-S3 固件技术方案 v2 | `docs/superpowers/specs/2026-09-06-esp32-device-agent-design.md` | 设计 spec（三平面归属、模块设计、预算、风险） |
| M1 实施计划 | `docs/superpowers/plans/2026-09-06-mibot-m1-uart-link.md` | 执行计划 + 全部评审遗留登记（前向约束/Task 3/Task 6-8 三张 M2 清单） |
| 本文 | `docs/mibot-design.md` | 现行全貌单一入口 |

代码：`main/boards/mibot/`（固件）、`test/host/`（宿主测试）、`scripts/mibot_uart_peer.py`（PC 对端）；分支 `feat/device-agent`。
