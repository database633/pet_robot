# Mibot ESP32-S3 固件技术方案 v2（AI 网关 + 运动安全执行器）

- 日期：2026-09-06
- 状态：设计评审中
- 架构依据：《Mibot 桌面机器人状态机与控制接口规范》（下称《规范》）
- 基线仓库：78/xiaozhi-esp32（分支 `feat/device-agent`）
- 修订记录：
  - v1：设备端 agent（loop 跑在 ESP32，pi agent-core 移植）。
  - **v2（当前）**：经确认遵循《规范》§1 三平面划分——**Agent loop、对话上下文、Skill 归 SF32LB53（openvela）**；本仓库承担 **ESP32-S3 侧：AI 网关（云端调用代理）+ 运动安全执行器**。v1 中 loop/context/tool-registry 的设计（§4.3~4.5）整体转移为 SF32 侧实现蓝图（见 §11），传输层与安全执行设计保留并改型。

## 0. 架构归属（已确认）

| 平面 | 运行位置 | 本仓库交付物 |
|---|---|---|
| 交互与 Agent | SF32LB53 / openvela | ❌ 不在本仓库（接口契约见 §3；实现蓝图见 §11） |
| 实时设备 | **ESP32-S3（本仓库）** | UART 协议栈、AI 网关、运动安全状态机、ToF/电机/舵机/相机驱动、`robot.*` 命令执行 |
| 云端 AI | LLM / ASR / TTS / 视觉 | 不交付；本仓库是其调用方 |

权威关系（遵循《规范》§1.1）：动作意图权威 = SF32 Agent；**执行与安全权威 = ESP32-S3**（可拒绝任何越界/过期/重复命令）；最终停止权威 = ESP32-S3 本地安全任务。云端 API Key 只存 ESP32 NVS，不经 UART 传递（规范 §7）。

## 1. 目标与非目标

### 1.1 目标（ESP32-S3 侧，v1）

1. **UART 链路栈**：《规范》§3.2 帧格式（0xAA55 / CRC-16/CCITT-FALSE / SEQ / 分片）、HELLO/HELLO_ACK、PING/PONG（500ms 周期，运动期间租约 1500ms 失效 → `BRAKING`）、DMA + 环形缓冲、控制帧与音频帧队列分离。
2. **AI 网关**（ESP32 持有全部云端配置与 TLS）：
   - `AI_REQUEST(0x40)` → 转发 `chat/completions`（鉴权注入）→ `AI_RESPONSE(0x41)` 回传 assistant 消息（content / tool_calls / 错误 / 结束）；
   - `AUDIO_UP(0x30)` PCM 流累积 → 云端 ASR → 文本经 `AI_RESPONSE` 回传；
   - `robot.speak` 命令 → 云端 TTS → `AUDIO_DOWN(0x31)` PCM 流下发 SF32 播放；
   - `robot.capture_image` → 相机 JPEG 上云 → 视觉结果 / 图片引用经 `CAMERA_RESULT(0x50)` 回传。
3. **运动安全执行器**：`robot.*` COMMAND(0x10) 的执行端——运动安全状态机（`MOTION_INIT/STANDBY/RUNNING/BRAKING/HOLD/FAULT`，《规范》§2.2）、四路 ToF 边缘检测（§2.3 参数表）、限幅/TTL/幂等校验、ACK(0x11)/NACK(0x12)、EVENT(0x20)/TELEMETRY(0x21) 上报。
4. **`robot.*` 命令集**（执行端语义，《规范》§4.2）：`get_status` / `move` / `rotate` / `stop` / `set_arm_pose` / `speak` / `capture_image` / `read_floor_sensors` 本地执行；`set_expression` 属 SF32 本地 LCD Skill，不经 ESP32——误发时 NACK `E_UNSUPPORTED`。参数限幅与错误码（《规范》§5.3）逐条落地。
5. 宿主端可测：帧编解码、CRC、分片、命令校验、信封序列化在 PC 上单元测试。

### 1.2 非目标（v1）

- Agent loop / 对话上下文 / Skill / 唤醒词 / VAD —— SF32 侧职责（《规范》§2.1 `LISTENING` 明确 VAD 在 SF32，音频以片段上行）
- BLE 配网、主动任务、多 Skill 并发规划（《规范》§8 第 7 步之后）
- 流式 LLM（`AI_RESPONSE` v1 以完整消息为单位回传，"文本增量"作为单条 delta 承载；SSE 列入 v2）
- OTA（默认关闭，《规范》§7；启用时需签名校验且禁止电机运行期间升级）

### 1.3 硬件前提

ESP32-S3（PSRAM ≥2MB）；UART 连接 SF32（921600 8N1，3.3V TTL，独立硬件 UART 不与 console 复用）；4 路 ToF（I²C，TCA9548A 分址或 XSHUT，`front_left/front_right/rear_left/rear_right`）；2×直流电机（TB6612）+ 2×舵机；相机（JPEG）；电池电压检测。**板卡无本地麦克风/喇叭**——音频全部经 UART 与 SF32 往返，AudioService 以 `dummy_audio_codec` 兼容初始化或整体不启用。

## 2. 系统数据流（一次语音回合）

```
SF32(openvela)                         ESP32-S3(本仓库)                     云端
────────────────                       ────────────────                    ─────
READY: 唤醒词命中 → LISTENING
MIC 采样+VAD ──AUDIO_UP(0x30)──▶ 累积 PCM(≤30s)
VAD 结束(eos) ───────────────────▶ POST /audio/transcriptions ──────▶ ASR
◀─AI_RESPONSE{kind:asr_text}──── 返回文本
THINKING: 拼请求(messages+tools)
──AI_REQUEST(0x40)──────────────▶ 注入 Authorization/URL ──────────▶ LLM
                                   (非流式, ≤8KB 响应上限)
◀─AI_RESPONSE{kind:assistant,──── 回传 content / tool_calls
   tool_calls:[...]}
  tool_call? → Skill 校验 → EXECUTING
──COMMAND(0x10) robot.move─────▶ 安全校验(限幅/TTL/幂等/边缘)
                                   │ 通过: 运动状态机执行
◀─ACK(0x11){ok,state,result}─────┘ 拒绝: NACK + E_* 错误码
  (工具结果并入上下文) ──AI_REQUEST(下一轮)──▶ …循环由 SF32 驱动…
  最终回复 → robot.speak COMMAND ──▶ POST /audio/speech ────────────▶ TTS
◀──AUDIO_DOWN(0x31) PCM 流──────── 16k/s16le 分帧下发
SPEAKING: 播放 + 表情同步；打断: 新唤醒词/急停
```

要点：**多轮循环完全由 SF32 驱动**——ESP32 每次只执行"一次 AI 请求"或"一条命令"，不保存对话上下文（《规范》§1）；"生成 Tool Call ≠ 已执行"，ESP32 的 ACK `completed` 是唯一执行凭证（《规范》§7）。

## 3. UART 接口契约（本仓库需实现/补充定义的部分）

《规范》§3.1/3.2/3.3/3.4 直接采用：帧格式、消息类型表、通用字段（`schema/msg_id/trace_id/command_id/created_at/expires_at/name/args`）、握手保活重试。以下为本方案补充明确的部分：

### 3.1 AI_REQUEST payload（0x40）

```json
{
  "schema": "mibot.ai.v1",
  "msg_id": "...", "trace_id": "...", "created_at": "...", "expires_at": "...",
  "kind": "chat",
  "request": { "messages": [...], "tools": [...], "stream": false, "temperature": 0.7 }
}
```

- ESP32 **不解释** `request` 内容，仅：注入 `Authorization: Bearer <key>` 与目标 URL（NVS 配置）、附加 `model` 字段（NVS 配置）、执行 HTTPS 调用。
- payload 超 4096 字节时使用帧分片（FLAGS bit3）重组；总上限 64KB（覆盖含长历史的请求），超限回 `AI_RESPONSE{kind:error, code:"E_PAYLOAD_TOO_LARGE"}`。
- TLS/内存策略沿用 v1 已验证设计：非流式、每请求新建连接、mbedTLS 缓冲入 PSRAM、同时刻仅一条云端连接、响应体 >8KB 中止。

### 3.2 AI_RESPONSE 事件种类（0x41）

```json
{"schema":"mibot.ai.v1","msg_id":"...","trace_id":"...","kind":"asr_text","text":"把音量调到50"}
{"schema":"mibot.ai.v1","...","kind":"assistant","message":{"content":"...","tool_calls":[{"id":"call_1","type":"function","function":{"name":"robot.move","arguments":"{...}"}}],"finish_reason":"tool_calls"}}
{"schema":"mibot.ai.v1","...","kind":"error","code":"E_CLOUD_TIMEOUT","message":"..."}
{"schema":"mibot.ai.v1","...","kind":"done"}
```

### 3.3 音频流（0x30/0x31）

按《规范》§3.6：首帧携带元数据（`pcm_s16le / 16000 / mono / 20ms / 640B / eos`），后续帧为纯 PCM。上行累积上限 30s；`robot.speak` 的 TTS 输出重采样至 16k s16le 后下发（重采样用工程既有 `esp_ae_rate_cvt`）；收到 `stop_audio` 命令或新唤醒即丢弃当前流。

## 4. ESP32-S3 模块设计（全部位于 `main/boards/mibot/`）

```
main/boards/mibot/
  mibot_board.cc            板卡注册（引脚/外设/启动自检），对接 xiaozhi Board 框架
  uart_link/{uart_link.h,frame_codec.cc,uart_task.cc}
  ai_gateway/{llm_proxy.cc,asr_client.cc,tts_client.cc,vision_client.cc}
  motion/{motion_fsm.cc,motor_tb6612.cc,servo.cc,tof_array.cc,command_exec.cc}
  audio_relay/audio_relay.cc
  camera/camera_capture.cc
```

（`frame_codec`、`command_exec` 的校验逻辑、信封序列化为纯函数，供宿主测试。）

### 4.1 uart_link——链路层

- `frame_codec.{h,cc}`：纯函数编解码——SOF 同步、VERSION/LENGTH 校验、CRC-16/CCITT-FALSE（覆盖 VERSION..PAYLOAD）、SEQ 单调性、分片重组；错误 → `NACK(E_PROTOCOL_CRC)`，不执行 payload（《规范》§3.2）。
- `uart_task.{h,cc}`：UART 事件任务（DMA + 环形缓冲）；两条独立队列——控制帧队列（有界、丢帧重传 ≤2 次）与音频帧队列（丢弃策略，绝不阻塞运动安全任务，《规范》§3.5）；HELLO/HELLO_ACK（3 次失败 → 通知上层进入 DEGRADED 语义事件）；PING/PONG 500ms；**运动期间 1500ms 无有效保活/租约刷新 → 直接触发 `BRAKING`**（不等待任何上层）。

### 4.2 ai_gateway——云端代理

| 组件 | 职责 | 沿用 v1 设计 |
|---|---|---|
| `llm_proxy.cc` | AI_REQUEST → HTTPS → AI_RESPONSE；鉴权/URL/model 注入；payload 分片重组；超时 30s、5xx/网络错误重试 1 次；`trace_id` 写入日志 | §v1 4.8 全部（TLS bundle、单连接、8KB 上限、取消语义改为"AI_REQUEST 过期"） |
| `asr_client.cc` | AUDIO_UP 累积（≤30s，PSRAM）→ WAV 构造 → multipart 上传 → 文本；超时 10s、重试 1 次 | §v1 4.6 全部（独立 `esp_opus_dec` 不再需要——上行即为 PCM，删除 Opus 解码步骤） |
| `tts_client.cc` | 文本 → `/audio/speech` → PCM（16k s16le 重采样）→ 边收边转 AUDIO_DOWN 分帧下发（流式，整段一次请求；句级切分降首音延迟列 v2）；`interruptible` 时可被 stop_audio/新唤醒中止 | 新增；供应商配置要求支持 pcm/wav 输出，避免引入 mp3 解码器 |
| `vision_client.cc` | JPEG 上传（`purpose` 透传）→ 结构化视觉结果 → CAMERA_RESULT；按需触发，无持续上传 | 新增 |

配置（复用 `Settings`，NVS 命名空间 `mibot`）：`llm_url/llm_key/llm_model`、`asr_url/asr_key/asr_model`、`tts_url/tts_key/tts_model/tts_voice`、`vision_url/vision_key`。串口 console 命令（`esp_console`，参照 `sensecap_watcher.cc:436` 模式）：`mibot show / mibot set llm.key sk-xxx / mibot ping`。

### 4.3 motion——运动安全执行器（本方案的硬实时核心）

**运动安全状态机**（《规范》§2.2）：

| 状态 | 进入条件 | 行为 |
|---|---|---|
| `MOTION_INIT` | 上电 | PWM/TB6612/ToF 初始化，舵机安全角 |
| `STANDBY` | 初始化完成 / 动作结束 | 电机停，接受新命令 |
| `RUNNING` | 合法 move/rotate/arm 命令 | 执行有界动作（每动作 TTL + 结束时间） |
| `BRAKING` | 边缘触发 / 急停 / 租约失效 / 停止命令 | 立即断 PWM，舵机保持 |
| `HOLD` | BRAKING 后待确认 | 等待 SF32 重新规划或人工确认 |
| `FAULT` | ToF 全失效 / 堵转 / 驱动异常 | 禁止一切运动，上报 EVENT |

**ToF 边缘检测**（独立 FreeRTOS 任务，20Hz 轮询四路，参数按《规范》§2.3 默认值：`edge_threshold_mm=50`、`edge_confirm_count=3`、`motion_slowdown_mm=120`、`sensor_timeout_ms=250`）：任意前方传感器连续 3 次超阈值 → **不经 UART、不等云端，本地直接 `BRAKING`** 并发 `EVENT edge_detected(critical)`；ToF 失效禁止新的前进动作；传感数据统一毫米 + `valid/quality`。

**命令执行（`command_exec.cc`）**——`COMMAND(0x10)` 的统一入口，校验顺序：CRC/版本（链路层已做）→ `E_DUPLICATE`（`command_id` 幂等，缓存最近结果，《规范》§3.4）→ `E_EXPIRED`（`expires_at`）→ 参数限幅（Schema 硬编码于本模块，越界 `E_INVALID_ARG`）→ 安全状态检查（`E_SAFETY_LOCK`/`E_BUSY`/`E_LOW_BATTERY`/`E_TOF_INVALID`）→ 执行。执行结果以《规范》§4.1 信封封装为 ACK payload：

```json
{"ok":true,"command_id":"cmd_...","state":"completed","result":{"estimated":false},"error":null}
{"ok":false,"command_id":"cmd_...","state":"rejected","result":null,"error":{"code":"E_SAFETY_LOCK","message":"前方检测到桌面边缘"}}
```

各命令执行归属：`move/rotate/stop/set_arm_pose/read_floor_sensors/get_status` → 本地（运动状态机/驱动/ToF）；`speak` → 转交 `tts_client` + AUDIO_DOWN；`capture_image` → 转交 `vision_client`；`set_expression` → NACK `E_UNSUPPORTED`（SF32 本地 LCD Skill 执行，无需经 ESP32，《规范》§4.2）。限幅（《规范》§4.2）：`linear_mm_s −120..120`、`angular_deg_s −90..90`、`duration_ms 50..5000` 默认 500、舵机 0..180 按机械限位收紧；`avoid_edge` 永远开启，无任何命令可关闭。

**并发与优先级**（《规范》§6）：运动命令不并发——新 `move/rotate` 到达时先收尾/拒绝旧动作（`E_BUSY` 或安全截断）；`stop` 可随时插入；舵机可与语音播放并行；优先级：急停/边缘/堵转/低压 > `robot.stop` > 语音会话 > Agent 命令 > 后台 > 表情/日志。

**遥测**：运动期间 5~10Hz、空闲 1Hz 的 `TELEMETRY(0x21)`（格式《规范》§5.1：双状态、`battery_mv`、四路 ToF、电机/舵机、Wi-Fi RSSI）。

### 4.4 audio_relay 与 camera

- `audio_relay.cc`：上行流管理（stream_id、eos、30s 截断标记）+ 下行流泵（TTS PCM → 20ms/640B 帧 → AUDIO_DOWN）；元数据首帧规则按 §3.3。
- `camera_capture.cc`：按需拍照（`width/height/quality`）→ JPEG → 上传；完整 JPEG 不经 UART 回传（《规范》§4.2）。

## 5. 一次语音回合的时序与预算

| 步骤 | 链路 | 预算 |
|---|---|---|
| 唤醒→LISTENING→AUDIO_UP 片段上传 | UART 921600（PCM 320kbit/s < 链路带宽） | 实时 |
| ASR 上传+识别 | ESP32→云 | 1~2s（超时 10s） |
| AI_REQUEST→LLM→AI_RESPONSE | SF32→UART→ESP32→云→回 | 1~3s/轮（超时 30s） |
| COMMAND→执行→ACK | UART 往返 | 控制帧 <10ms + 动作时长（TTL 有界） |
| speak→TTS→AUDIO_DOWN | ESP32→云→UART | 首音频 1~2s |
| loop 迭代次数 | SF32 决策 | 上限由 SF32 侧设定（建议 ≤8，对应 v1 蓝图） |

## 6. 资源预算（S3 / 8MB PSRAM）

| 项 | 预算 | 说明 |
|---|---|---|
| 内部 RAM | ~45KB 峰值 | 单条 TLS（同时刻仅一条云端连接：ASR/LLM/TTS/视觉串行） |
| PSRAM：上行 PCM 累积 | ≤960KB | 30s × 32KB/s |
| PSRAM：TTS 下行缓冲 | ≤128KB | 流式转帧，环形 |
| PSRAM：LLM 请求/响应 | ≤64KB / ≤8KB | 分片重组 / 解析上限 |
| PSRAM：JPEG | ≤512KB | 按分辨率/质量配置 |
| 任务 | uart_link 6KB / motion 4KB（高优先级）/ ai_gateway 8KB / tof 3KB | 运动安全任务优先级最高 |
| UART DMA 环形缓冲 | RX 16KB / TX 16KB | 控制与音频分队列 |

防御规则：任何 PSRAM 分配失败 → 相应流降级（拒新命令 `E_BUSY` / 丢弃音频片段），**运动安全路径永不依赖可失败的大块分配**。

## 7. 错误处理矩阵

| 阶段 | 错误 | 表现 | 恢复 |
|---|---|---|---|
| UART | CRC/长度/版本错 | `NACK(E_PROTOCOL_CRC)`，payload 不执行 | 丢帧重传 ≤2 次（仅控制帧） |
| UART | 运动期间 1500ms 无保活 | **本地 `BRAKING`** + `EVENT uart_timeout` | 链路恢复后 SF32 重新握手 |
| UART | HELLO 3 次失败 | `EVENT`（SF32 进 DEGRADED） | 持续重试 |
| 命令 | 重复/过期/越界/安全拒绝 | ACK `state:rejected` + `E_DUPLICATE/E_EXPIRED/E_INVALID_ARG/E_SAFETY_LOCK/E_BUSY/E_LOW_BATTERY` | 模型/SF32 据信封自我纠正 |
| ToF | 边缘 / 传感器失效 | 本地刹车 + `EVENT edge_detected/tof_invalid` | ESP32 全权处理，不等待任何一方 |
| 电机/舵机 | 堵转 / 限位 / 电源异常 | `EVENT motor_stall(critical)/servo_limit` + `FAULT`；`E_SERVO_POWER` 后不重试 | 人工恢复流程 |
| 云端 | ASR/LLM/TTS 超时或 4xx/5xx | `AI_RESPONSE{kind:error}`（`E_CLOUD_TIMEOUT` 等） | SF32 决定重试/降级；`cloud_disconnected` 期间仅保留本地停止/唤醒（《规范》§5.2） |
| 音频 | 上行 >30s | 截断标记随文本回传 | SF32 提示用户 |
| 打断 | `stop_audio` / 新唤醒 / 急停 | 丢弃当前音频流；`emergency=true` 直接最高优先级刹车 | 立即生效 |

## 8. 测试方案

### 8.1 宿主端单元测试（`test/host/`，PC 上运行）

被测纯逻辑：`frame_codec`（CRC 向量、SOF 重同步、分片重组、SEQ 回绕）、`command_exec` 校验链（幂等/TTL/限幅/安全拒绝，逐错误码断言）、信封序列化、AI_RESPONSE 解析。设备无关，CI 可接。

### 8.2 设备端验证（直接采用《规范》§8 最小可验收闭环顺序）

| 步骤（《规范》§8） | 通过标准 |
|---|---|
| 1. HELLO/CRC/PING/TELEMETRY | 链路稳定 1h 无 CRC 错误堆积；保活丢失 1500ms 可复现 |
| 2. ToF + 边缘 | 人工遮挡任一前方传感器，**本地**立即刹车（拔掉 UART 仍生效） |
| 3. stop/move/set_arm_pose | 限速、TTL 过期、重复命令幂等、**串口拔线刹车**全部验证 |
| 4. 音频上下行 | SF32 麦克风 PCM → 云端 ASR/TTS → PCM 下行，半双工可用 |
| 5. 表情与 speak 状态同步 | LCD 表情反映真实播放/执行状态，非发送状态 |
| 6. capture_image | 按需 JPEG 上云，视觉结果回传 |
| 7. 主动任务/BLE/多轮规划 | 后续版本 |

完成定义（《规范》§8）：**断云状态下**机器人仍能本地停止、识别 ToF 异常、拒绝危险动作；云恢复后 SF32 经同一套命令接口查询状态并发起有界动作；LCD 表情来自设备实际状态。

## 9. 里程碑（= 《规范》§8 顺序，每步独立可验收）

1. **M1**：`uart_link` 全栈（帧/CRC/分片/HELLO/PING/TELEMETRY）+ mibot 板卡骨架 + `Settings`/console 配置。DoD：§8.2 步骤 1 通过。
2. **M2**：ToF 阵列 + 边缘检测 + `BRAKING`。DoD：§8.2 步骤 2（含断 UART 刹车）。
3. **M3**：`command_exec` + 电机/舵机驱动 + `move/rotate/stop/set_arm_pose/get_status/read_floor_sensors`。DoD：§8.2 步骤 3。
4. **M4**：`ai_gateway`（asr/llm）+ `audio_relay` 上行。DoD：§8.2 步骤 4 前半（ASR 闭环）。
5. **M5**：`tts_client` + AUDIO_DOWN + speak。DoD：§8.2 步骤 4 后半 + 步骤 5。
6. **M6**：`camera_capture` + `vision_client` + CAMERA_RESULT。DoD：§8.2 步骤 6。
7. **M7**（后续版本）：BLE 配网、主动任务、多轮规划增强。

## 10. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 921600 UART 音频带宽拥塞（控制帧与 PCM 争抢） | 控制帧延迟影响安全 | 物理分队列 + 音频帧可丢弃策略；《规范》§3.1 预留 RTS/CTS 升级位 |
| 运动安全任务被 UART/网络任务抢占 | 刹车延迟 | motion 任务最高优先级 + ToF 检测独立任务；BRAKING 路径零动态分配 |
| `expires_at` 时钟不一致（SF32 与 ESP32 时基） | 好命令被拒 | HELLO_ACK 携带时钟参数（《规范》§3.3）；TTL 以 ESP32 收包时刻起算的宽松校验 |
| TTS 供应商不支持 16k PCM 输出 | 引入解码器依赖 | 配置约束（§4.2）；必要时加 `esp_ae_rate_cvt` 重采样（工程已用） |
| AI_REQUEST 大 payload 分片重组失败 | 丢轮次 | 总上限 64KB + 分片超时回 `error`；SF32 可重发 |
| 与上游 xiaozhi 演进冲突 | 合并成本 | 全部新代码集中于 `main/boards/mibot/`；对上游共享文件零修改（板卡机制天然隔离） |

## 11. SF32 侧（openvela）实现蓝图归属

v1 方案中以下设计**不作废，转移为 SF32 侧实现蓝图**（C++ 可用，宿主测试方案原样适用）：

- **AgentLoop / AgentContext / ToolRegistry**（v1 §4.3~4.5，pi agent-core 移植）：运行于 SF32；工具执行 = Skill 校验 → UART COMMAND → ACK 信封并入上下文。`LlmTransport` 接口的设备实现变为"UART AI_REQUEST 客户端"。
- **统一返回信封与 E_* 错误码**（v1 §4.5）：即《规范》§4.1/§5.3，双端一致。
- **扩展点**：远程 MCP 工具（`RegisterRemoteMcpTools`）与 Skill 机制（prompt 片段 + 工具组）均属 SF32 侧——SF32 有 Wi-Fi/BLE 之外的连接能力时亦可由 ESP32 网关代理；工具容量预算（≤8KB/32 个）随之归 SF32 侧执行。
- **宿主端单测**：loop/上下文/信封的 mock 测试在 PC 上开发，目标平台仅影响传输层。

## 附：改动文件全景（v2）

```
新增  main/boards/mibot/             （本仓库全部新代码：uart_link / ai_gateway / motion / audio_relay / camera）
新增  test/host/                     （宿主端测试：frame_codec / command_exec / 信封）
修改  main/CMakeLists.txt            （若板卡注册机制要求，通常无需——板卡目录自包含）
参考  docs/superpowers/specs/        （本 spec；v1 内容作为 SF32 侧蓝图保留于 §11）
上游  78/xiaozhi-esp32 共享代码       （Settings / esp_console 模式 / esp_http_client / TLS 配置，按既有用法引用，不修改）
```
