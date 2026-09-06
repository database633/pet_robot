# ESP32 设备端 Agent（Device Agent）技术方案

- 日期：2026-09-06
- 状态：设计评审中
- 蓝图来源：[earendil-works/pi](https://github.com/earendil-works/pi) `packages/agent`（agent-loop.ts / AgentContext / StreamFn 架构的 C++ 移植）
- 基线仓库：78/xiaozhi-esp32（分支 `feat/device-agent`）

## 1. 目标与非目标

### 1.1 目标（v1）

1. 在 ESP32 固件内实现 agent loop：设备持有对话状态、工具定义与工具执行，循环推进决策；**不使用小智官方服务器的 agent loop**。
2. 云端只提供三个 OpenAI 兼容 API：ASR（`/audio/transcriptions`）、LLM（`/chat/completions`）、（未来）TTS。供应商（DeepSeek / Qwen / GLM / SiliconFlow / Groq 等）是配置项，不是代码分支。
3. 交互形态：**语音输入 + 文字输出**。唤醒词 → 录音 → 云端 ASR → 设备端 agent loop → 回复文字/情绪/工具调用过程显示在屏幕。不实现语音回复。
4. 设备工具（音量、亮度、拍照、状态查询、板卡自定义 GPIO/电机等）与官方服务器模式共享同一份注册代码，不出现两套定义。
5. 官方服务器模式完整保留，通过 Kconfig 编译期开关切换，可随时回退。

### 1.2 非目标（v1 明确不做）

- TTS 语音回复（架构预留插入位，见 §11）
- LLM 流式输出（SSE 增量解析，见 §11）
- Realtime 端到端语音模型
- 对话历史持久化（跨重启）
- 多模态视觉对话（"拍照→视觉模型"作为 M4 的扩展工具，不是基础能力）
- 免唤醒连续对话（回复后自动保持聆听窗口）——v1 每轮对话由唤醒词/按键发起，连续对话列入 §11

### 1.3 硬件前提

- ESP32-S3 或 ESP32-P4，带 PSRAM（≥2MB 可用）。无 AEC 硬件要求（设备不说话，全双工无意义）。
- 麦克风 + 屏幕（Display 组件）。无屏板子可运行但只能通过串口看回复（M1 阶段即此形态）。

## 2. 总体架构

### 2.1 与 pi 的映射

| pi（TypeScript, `packages/agent/src/`） | 本方案（C++, `main/agent/`） | 取舍 |
|---|---|---|
| `agent-loop.ts` `runLoop` | `agent_loop.cc` `AgentLoop::Run()` | 砍掉 steering / follow-up 队列 / compaction，保留核心循环 |
| `types.ts` `AgentContext` | `agent_context.h` `AgentContext` | 消息列表 + 工具表 + system prompt，纯状态无 IO |
| `stream-fn.ts` `StreamFn`（LLM 调用边界） | `llm_transport.h` `LlmTransport`（纯虚接口） | 设备上是 HTTPS 实现；宿主测试是 mock 实现 |
| 事件流（`agent_start`/`turn_start`/`tool_call`…） | `AgentCallbacks`（std::function 回调集） | loop 不碰 UI，Display 只消费事件 |
| `pi-ai` 多 provider 统一 API | `llm_openai.cc` 单一 OpenAI 兼容实现 | v1 只此一家形状；provider 差异 = 配置差异 |

### 2.2 一次语音回合的数据流

```
[本地]  唤醒词(ESP-SR) → kDeviceStateListening → Opus 帧累积(60ms/帧, 16k/mono)
        VAD 结束(on_vad_change=false)
[云端]  ① ASR：Opus→解码→内存WAV → POST /audio/transcriptions → {text}     ~1-2s
[本地]  ② agent loop（kDeviceStateThinking）：
          ┌ 循环 ≤8 轮 ────────────────────────────────────────────┐
          │ POST /chat/completions（messages+tools, stream:false） │  ~1-3s/轮
          │ ← tool_calls? → ToolRegistry 本地执行（同步、毫秒级）    │
          │   tool result 回填 → 下一轮                              │
          └ 无 tool_calls → 最终回复文本 ────────────────────────────┘
[本地]  ③ Display：回复上屏(SetChatMessage) + 情绪(SetEmotion) + 过程(工具名+参数)
        → kDeviceStateIdle
```

端到端延迟预算：单轮工具调用约 3~6s；无工具约 2~4s。

### 2.3 模式开关

新增 Kconfig 选项 `CONFIG_USE_DEVICE_AGENT`（默认关）。开启后：

- Application 的音频出口（`application.cc:238` `PopPacketFromSendQueue` 循环）由 `protocol_->SendAudio()` 分流为 `device_agent_->OnOpusPacket(std::move(packet))`；
- `AudioServiceCallbacks`（`audio_service.h:78`：`on_wake_word_detected` / `on_vad_change`）在 DeviceAgent 模式下转发给 DeviceAgent 而非协议会话逻辑；
- WebSocket/MQTT 协议代码一行不改，仅不实例化。

## 3. 集成点清单（全部为现有代码中已核实的位置）

| # | 现有代码 | 改动 |
|---|---|---|
| 1 | `main/Kconfig.projbuild` | 追加 `menu "Device Agent"` + `CONFIG_USE_DEVICE_AGENT`（bool，默认 n） |
| 2 | `main/CMakeLists.txt` | SRCS 追加 `agent/*.cc` |
| 3 | `application.cc:238` 音频出口处 | 按 Kconfig 分流（见 §2.3） |
| 4 | `application.cc` 绑定 `AudioServiceCallbacks` 处 | 唤醒/VAD 事件转发 DeviceAgent |
| 5 | `main/mcp_server.cc:33` `McpServer::AddCommonTools()` | 实现体迁移到 `tool_registry.cc`，原函数改为委托（见 §4.5） |
| 6 | `main/device_state.h` | 追加 `kDeviceStateThinking`（纯新增枚举值，置于 Listening 与 Speaking 之间） |
| 7 | `sdkconfig.defaults` | 追加 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`（mbedTLS 缓冲入 PSRAM，缓解 TLS 内存压力） |

## 4. 模块设计（新增 `main/agent/`）

### 4.1 文件与职责

| 文件 | 职责 | 依赖 |
|---|---|---|
| `agent_types.h` | `ChatMessage` / `ToolCall` / `AssistantMessage` / `AgentEvent` 等纯数据类型 | cJSON |
| `agent_context.h/.cc` | 消息历史管理：追加、裁剪、序列化为请求 `messages` 数组 | cJSON |
| `tool_registry.h/.cc` | 工具定义与执行；`RegisterCommonTools()`；OpenAI `tools` 数组序列化 | cJSON |
| `llm_transport.h` | `LlmTransport` 纯虚接口（pi `StreamFn` 的对应物） | — |
| `llm_openai.cc/.h` | HTTPS 实现：请求构造、非流式响应解析、超时/重试/取消 | esp_http_client, cJSON |
| `asr_client.h/.cc` | Opus 帧累积 → 解码 → 内存 WAV → multipart 上传 → 文本 | esp_http_client, esp_opus_dec |
| `agent_loop.h/.cc` | 核心循环（pi `runLoop` 移植）：迭代、事件、取消 | 以上全部 |
| `device_agent.h/.cc` | 编排：任务/队列/状态机映射/Display/打断/配置 | audio_service, display, settings |

### 4.2 核心数据类型（`agent_types.h`）

```cpp
struct ToolCall {
    std::string id;          // "call_abc123"（回填 tool 消息时必需）
    std::string name;        // "self.audio_speaker.set_volume"
    std::string arguments;   // 原始 JSON 字符串，执行前才解析
};

struct AssistantMessage {
    std::string content;                 // 可为空（纯工具调用轮）
    std::vector<ToolCall> tool_calls;    // 空即循环终止
    std::string finish_reason;           // "tool_calls" | "stop" | 其他
    bool error = false;                  // 网络/解析失败
    std::string error_message;
};

struct ChatMessage {
    enum class Role { kSystem, kUser, kAssistant, kTool } role;
    std::string content;
    std::vector<ToolCall> tool_calls;    // 仅 assistant
    std::string tool_call_id;            // 仅 tool
};

enum class AgentEventType { kTurnStart, kToolCall, kToolResult, kReply, kError, kAborted };
struct AgentEvent { AgentEventType type; std::string text; };  // text: 工具名/结果摘要/回复/错误
```

### 4.3 LlmTransport 接口与 AgentLoop（pi 核心的移植）

```cpp
// llm_transport.h —— 宿主测试的注入点
class LlmTransport {
public:
    virtual ~LlmTransport() = default;
    // 阻塞调用；cancel 置位后应尽快返回 error=true（error_message="aborted"）
    virtual AssistantMessage Chat(const std::string& request_json,
                                  const std::atomic<bool>& cancel) = 0;
};

// agent_loop.h
struct AgentLoopConfig {
    int max_iterations = 8;
    std::chrono::seconds llm_timeout{30};
};

class AgentLoop {
public:
    AgentLoop(LlmTransport& transport, ToolRegistry& registry, AgentContext& context);
    using EventSink = std::function<void(const AgentEvent&)>;
    void SetEventSink(EventSink sink);
    // 阻塞；内部将 user_text 追加到 context 后执行循环
    AssistantMessage Run(const std::string& user_text, const std::atomic<bool>& cancel);
};
```

循环体（与 pi `runLoop` 逐段对应，砍掉 steering/compaction）：

```
Run(user_text):
    context.AppendUser(user_text)
    emit(kTurnStart)
    for i in 1 .. max_iterations:
        resp = transport.Chat(context.SerializeRequest(), cancel)
        if resp.error:                      # 网络/解析失败
            emit(kError, resp.error_message); return resp
        context.AppendAssistant(resp)
        if resp.tool_calls.empty():         # finish_reason == "stop"
            emit(kReply, resp.content); return resp
        for tc in resp.tool_calls:          # 串行执行；本地工具毫秒级，远程工具秒级（§4.5）
            emit(kToolCall, tc.name + " " + tc.arguments)
            result = registry.Execute(tc.name, tc.arguments, cancel)   # 永不抛异常；受 per-tool 超时约束
            context.AppendToolResult(tc.id, result)
            emit(kToolResult, 摘要)
    # 达到 max_iterations 仍未收敛
    emit(kError, "max_iterations"); return 固定兜底文本
```

要点：

- **工具错误不终止对话**：`ToolRegistry::Execute` 捕获一切异常/未知工具/参数解析失败，包装为 `{"isError": true}` 形态的文本结果回填，让模型自我纠正（pi 同款行为）。
- **取消**：`cancel` 原子标志贯穿 transport（HTTP abort）、loop（轮间检查）与工具执行（`Execute` 内部长操作分段检查）；置位后循环退出并 `emit(kAborted)`，context 回滚本次 `user` 消息（工具副作用不回滚，见 §7）。
- **迭代上限 8**：超出后不继续调模型，直接返回兜底话术并上屏，防止成本失控与死循环。

### 4.4 AgentContext：历史与裁剪

- 持有 `std::vector<ChatMessage>`（PSRAM 侧分配，走 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 的自定义分配器或整体容量告警，见 §6）。
- system prompt 为第 0 条，永不裁剪。
- 序列化规则：
  - `system` → `{"role":"system","content":...}`
  - `user` → `{"role":"user","content":...}`
  - `assistant` → `content` + 可选 `tool_calls:[{id,type:"function",function:{name,arguments}}]`
  - `tool` → `{"role":"tool","tool_call_id":...,"content":...}`
- 请求体顶层：`{"model": <cfg>, "messages": [...], "tools": [...], "stream": false}`。
- 裁剪策略：追加前估算文本字节数（UTF-8 长度），超过 32KB 则从最旧的一条**完整轮次**（user+assistant+其 tool 消息）开始丢弃，直到满足。一个轮次要么整体保留要么整体丢弃，避免出现悬空 `tool_call_id`（部分供应商会 400）。
- 响应内存上限：`Chat()` 收到的 HTTP body 超过 8KB 即中止解析报错（正常含工具定义的回执远小于此；防御性上限防内存尖峰）。

### 4.5 ToolRegistry 与 McpServer 共享重构

现状：`McpServer::AddCommonTools()`（`mcp_server.cc:33`）注册 `self.get_device_status` / `self.audio_speaker.set_volume` / `self.screen.set_brightness` / `self.camera.take_photo`，各板卡可能再注册自己的工具。

重构（对 `mcp_server` 的对外行为零改动）：

1. 新建 `tool_registry.h`：
   ```cpp
   struct ToolDef {
       std::string name, description;
       std::string parameters_json;   // JSON Schema（与 MCP inputSchema 同形状）
       std::function<std::string(const cJSON* args)> callback;  // 返回文本结果
   };
   class ToolRegistry {
   public:
       void AddTool(ToolDef def);
       const ToolDef* Find(const std::string& name) const;
       std::string SerializeOpenAiTools() const;   // [{"type":"function","function":{...}}]
       std::string Execute(const std::string& name, const std::string& arguments_json,
                           const std::atomic<bool>& cancel) const;   // 内置 per-tool 超时（默认 5s）
   };
   void RegisterCommonTools(ToolRegistry& registry);   // 自 mcp_server.cc:33 迁入
   ```
2. `McpServer::AddCommonTools()` 改为：构造内部 `ToolRegistry` → 调 `RegisterCommonTools()` → 将 `ToolDef` 适配为现有 `McpTool` 对象（`ToolDef::parameters_json` 直接作为 `McpTool::to_json()` 的 `inputSchema`，两者同构）。板卡自定义工具走原 `AddTool` 路径不受影响。
3. DeviceAgent 持有自己的 `ToolRegistry` 实例，同样调 `RegisterCommonTools()`；若板卡侧将来提供 `RegisterBoardTools(ToolRegistry&)` 钩子则两边同时受益（v1 只保证公共四件套共享）。

`Execute` 语义（返回统一采用 §4.5.1 的 Mibot 信封）：工具回调返回结构化结果，registry 序列化为信封 JSON 文本作为 tool 消息 content 回填模型——成功 `{"ok":true,"command_id":"...","state":"completed","result":{...},"error":null}`；未知工具 → `state:"rejected"` + `error.code:"E_UNSUPPORTED"`；参数缺失/非法 → `E_INVALID_ARG`；回调抛异常 → `error.message` 携带摘要。模型依据错误码自我纠正（如收到 `E_SAFETY_LOCK` 应停止移动并向用户解释）。`command_id` 由 registry 生成作幂等键，per-tool 超时映射为 `expires_at`。执行期间分段检查 `cancel` 并施加 per-tool 超时（默认 5s）——本地工具用不到，但这是远程工具（下）与秒级运动动作能安全接入的前提，也是 barge-in 在工具执行阶段仍然生效的前提。

**远程工具扩展点（设备作为 MCP Client，v2）**：`ToolDef` 的执行形态统一为"入参 JSON → 结果文本"，天然可封装远程调用——`RegisterRemoteMcpTools(registry, endpoint)`（v2）对远端 MCP 服务器（Streamable HTTP 传输，本质为 HTTP POST + JSON-RPC）执行 `initialize`/`tools/list`，将每个远程工具包装为一个 `ToolDef`，其 callback 内完成 `tools/call` 并取回文本结果。**对 loop 与模型而言远程工具与本地工具完全同构，loop 零改动**。代价是远程工具为秒级耗时，故依赖 `Execute` 的超时与 cancel（见上）。工具容量预算：`SerializeOpenAiTools()` 输出 ≤8KB（约 32 个工具），超限拒绝注册并经串口告警——防止远端工具目录撑爆请求体与 token 成本。

#### 4.5.1 Mibot 机器人工具集（对齐《Mibot 桌面机器人状态机与控制接口规范》§4）

安全原则（规范 §0/§4，不可协商）：模型只能表达**高层意图**——工具参数只含目标速度/角度/时长，不含 PWM、寄存器、I²C 地址；限幅、TTL、ToF 边缘保护、急停全部由 ESP32 本地安全状态机执行，任何模型输出都不能关闭 `avoid_edge`；"模型生成了 Tool Call" ≠ "已执行"，回填给模型的必须是真实执行结果（`ACK completed`），表情显示真实状态而非发送状态（规范 §1.1/§7）。

信封与错误码沿用规范 §4.1/§5.3：`E_INVALID_ARG` / `E_EXPIRED` / `E_DUPLICATE` / `E_BUSY` / `E_SAFETY_LOCK` / `E_TOF_INVALID` / `E_MOTOR_STALL` / `E_SERVO_LIMIT` / `E_LOW_BATTERY` / `E_UART_TIMEOUT` / `E_CLOUD_TIMEOUT` / `E_PROTOCOL_CRC` / `E_UNSUPPORTED`。

| 工具 | 参数要点（Schema 内置 min/max，模型自限幅） | 执行归属 |
|---|---|---|
| `robot.get_status` | 无参 | 返回 behavior/motion 状态、`tof`、`battery_mv`、`motor`、`servo`、`last_event` |
| `robot.move` | `linear_mm_s` −120..120、`angular_deg_s` −90..90、`duration_ms` 50..5000（默认 500）、`direction` | 本地运动安全状态机 |
| `robot.rotate` | `angle_deg`、`speed_deg_s`、`timeout_ms`；无编码器时结果带 `estimated=true` | 本地运动安全状态机 |
| `robot.stop` | `reason`、`emergency`（true → 最高优先级刹车，不等待当前动作） | 本地运动安全状态机 |
| `robot.set_arm_pose` | `left_deg/right_deg` 0..180（按机械限位收紧）、`duration_ms`、`hold_ms`；`E_SERVO_POWER` 后不得重试 | 本地运动安全状态机 |
| `robot.set_expression` | `name` ∈ {idle, listening, thinking, happy, confused, speaking, warning, error}、`duration_ms`、`intensity` | 双 MCU：SF32 LCD；单 MCU：本机 `Display` |
| `robot.speak` | `text` ≤500 字、`voice`、`interruptible` | v1 无 TTS：映射为屏幕显示并如实返回；接入 TTS 后由 ESP32 请求、SF32 播放 |
| `robot.capture_image` | `purpose`、`width`、`height`、`quality`；按需拍照，JPEG 上云后返回结构化视觉结果 | ESP32 相机 + 云端视觉 |
| `robot.read_floor_sensors` | 无参，诊断用；不能据此关闭边缘保护 | 本地 ToF 读取 |

工程归属：`robot.*` **不进** `RegisterCommonTools()` 公共集，由 Mibot 板卡目录（`main/boards/mibot/`：底盘/ToF/舵机驱动 + 运动安全状态机 + 工具注册）以板级工具形式注册——与 xiaozhi 既有"板卡自带工具"机制一致；通用板卡仍注册公共四件套。工具回调内部要么直接调本地安全状态机 API（单 MCU），要么封装为 UART `COMMAND` 帧（0x10/0x11：幂等 `command_id`、`expires_at`、ACK/NACK）——对 ToolRegistry 透明。并发约束（规范 §6"运动 Tool 不并发"）由 loop 的串行执行天然满足；打断后新回合的运动命令到达时由设备端先收尾旧动作（安全状态机职责）。表情通道统一：Mibot 板卡上禁用 §4.7 的 `[emotion:...]` 标记机制，表情一律由 `robot.set_expression` 工具或真实执行状态驱动；通用板卡保留标记机制。

### 4.6 AsrClient

音频来源与格式（已核实）：`AudioService` 麦克风链路为 `MIC → Audio Engine → Opus 编码 → Send Queue`，包类型 `AudioStreamPacket{sample_rate, frame_duration, timestamp, payload}`（`protocol.h:10`），编码参数 16kHz / mono / 16bit / 60ms 帧（`AS_OPUS_ENC_CONFIG`）。

流程：

1. `OnOpusPacket(AudioStreamPacket&&)`：kListening 状态下将 `payload` 追加进 PSRAM 累积缓冲；超过 30s（≈960KB PCM / ≈500 帧则停止累积并置 `truncated` 标记，仍继续处理前 30s。
2. VAD 结束（`on_vad_change(false)`）触发处理：
   - 用**独立的 `esp_opus_dec` 解码器实例**（不复用 `AudioService` 的解码器，避免与提示音播放互踩）将累积的 opus 帧逐帧解码为 PCM；
   - 内存构造 WAV：44 字节 RIFF 头 + PCM（16000Hz / 16bit / mono，`BytePerSec=32000`）；帧数为 0 则直接回到 Listening；
   - `multipart/form-data` 上传（boundary 手工拼接，总长可预知故用固定 Content-Length）：
     ```
     --<B>\r\n
     Content-Disposition: form-data; name="model"\r\n\r\n<asr_model>\r\n
     --<B>\r\n
     Content-Disposition: form-data; name="file"; filename="audio.wav"\r\n
     Content-Type: audio/wav\r\n\r\n
     <WAV bytes>\r\n
     --<B>--\r\n
     ```
   - 解析响应 `{"text": "..."}`；超时 10s，瞬时错误重试 1 次。
3. `truncated` 时在最终上屏文字后附提示（"（语音超过30秒已截断）"）。

### 4.7 DeviceAgent 编排

任务模型：

- 专属 FreeRTOS task：栈 8KB、优先级 4（低于音频任务），事件队列（`QueueHandle_t`，深度 8）驱动，事件类型 `{kWakeWord, kVadEnd, kOpusPacket(仅指针), kCancel, kSerialText}`。
- HTTP/cJSON/解码全部在该任务内串行执行；Application 与 AudioService 的实时性不受阻塞。
- `kOpusPacket` 仅在 `kDeviceStateListening` 有效：Thinking/Idle 期间到达的包直接丢弃（不入累积缓冲），send queue 由既有 drain 逻辑（`application.cc:898`、`application.cc:1090`）清空，防止积压。

状态机映射（复用 `SetDeviceState`，`application.h:78`）：

| DeviceAgent 内部阶段 | DeviceState |
|---|---|
| 空闲 | `kDeviceStateIdle` |
| 唤醒后录音中 | `kDeviceStateListening` |
| ASR 上传 + agent loop | `kDeviceStateThinking`（新增枚举值） |
| 结果上屏 | 回复写入后立即转 `kDeviceStateIdle`（v1 无 Speaking） |

Display 消费（只调现有接口）：

- ASR 文本：`SetChatMessage("user", text)`
- 工具调用：`SetStatus("🔧 self.audio_speaker.set_volume(50)")`（kToolCall/kToolResult 事件）
- 最终回复：`SetChatMessage("assistant", reply)`；情绪：`SetEmotion(...)`（由 system prompt 约定模型在回复尾部附 `[emotion:happy]` 类标记，解析后剥离；失败则用默认表情，不阻塞。Mibot 板卡上禁用该标记机制，见 §4.5.1）
- 提示音：`PlaySound` 沿用现有 `Lang::Sounds` 资源

打断（barge-in 替代逻辑，纯取消无音频清理）：

- `kDeviceStateThinking` 期间收到 `on_wake_word_detected` 或板卡按键事件（按钮映射沿用各板卡现有配置）→ 投递 `kCancel` → `cancel` 原子置位 → HTTP abort / loop 轮间退出 → 本次 user 消息从 context 回滚 → `emit(kAborted)` → 回 `kDeviceStateListening`。
- `kDeviceStateListening` 期间再次唤醒 → 忽略（已在听）。

配置（复用 `Settings`，NVS 命名空间 `agent`，`settings.h:9`）：

| 键 | 示例 |
|---|---|
| `asr_url` / `asr_key` / `asr_model` | `https://api.siliconflow.cn/v1/audio/transcriptions` / `sk-…` / `FunAudioLLM/SenseVoiceSmall` |
| `llm_url` / `llm_key` / `llm_model` | `https://api.deepseek.com/v1/chat/completions` / `sk-…` / `deepseek-chat` |
| `sys_prompt` | 存 assets 文件（体积大、可随固件资产管线分发），NVS 仅存覆盖项 |

串口配置命令：`esp_console` REPL，参照 `main/boards/sensecap-watcher/sensecap_watcher.cc:436` 的既有模式注册：

```
agent show                     # 列出配置（key 只显示前 6 位）
agent set llm.key sk-xxx
agent set llm.url https://...
agent test "帮我把音量调到50"   # 跳过语音直接跑一轮 loop（M1 的入口）
```

### 4.8 LlmOpenaiTransport 实现细节

- `esp_http_client`，每请求新建/销毁 client（v1 简单正确；TLS 握手 ≈300-500ms 可接受，keep-alive 复用列入 §11）。
- 内存：`buffer_size`/`buffer_size_tx` 2KB；`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` 使 mbedTLS 缓冲尽量落 PSRAM；**同一时刻全局仅一条 TLS 连接**（ASR、LLM 串行，loop 每轮串行）。
- 请求头：`Authorization: Bearer <key>`、`Content-Type: application/json`；TLS 证书校验使用 ESP x509 证书 bundle（`crt_bundle_attach`，IDF `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` 默认开启）——三个服务均为公网 HTTPS，禁止任何跳过校验的退化配置。
- 非流式响应解析（cJSON）：
  ```json
  {"choices":[{"message":{"role":"assistant","content":null,
    "tool_calls":[{"id":"call_abc","type":"function",
      "function":{"name":"self.audio_speaker.set_volume","arguments":"{\"volume\":50}"}}]},
    "finish_reason":"tool_calls"}],
   "error":{"message":"..."}}
  ```
  - `arguments` 是 **JSON 字符串**（不是内嵌对象），原样存入 `ToolCall::arguments`，执行时才解析；
  - `content` 为 `null` 时置空串；
  - 顶层有 `error` 或 HTTP ≥400 → `error=true`；
  - HTTP 429/5xx 与传输错误重试 1 次（间隔 500ms），4xx 不重试。
- 取消：读 body 循环中每次 `esp_http_client_read` 前检查 `cancel`，置位则 `esp_http_client_close` 并返回 aborted。

## 5. 一次完整语音回合的时序（编号步骤）

1. 唤醒词命中 → DeviceAgent 进 `kDeviceStateListening`，开始累积 opus 包（提示音 `PlaySound` 照旧）。
2. 用户说完 → VAD 下降沿 → 投递 `kVadEnd`，停累积。
3. ASR：解码→WAV→上传→文本（超时 10s）。失败：提示音+`SetStatus("语音识别失败")` → 回 Listening（重新说）。
4. 文本上屏（user 气泡），进 `kDeviceStateThinking`。
5. agent loop：每轮一次 HTTPS（超时 30s/轮，≤8 轮）；工具同步执行，事件上屏。
6. 最终回复上屏（assistant 气泡 + 情绪）→ `kDeviceStateIdle`。
7. 任意时刻（步骤 4~6 中）唤醒/按键 → 取消全部在途请求，回步骤 1。

## 6. 资源预算

| 项 | 预算 | 说明 |
|---|---|---|
| 内部 RAM | ~45KB 峰值 | 单条 TLS 连接握手+会话（mbedTLS 尽量入 PSRAM 后更低） |
| PSRAM：ASR 缓冲 | ≤960KB | 30s × 32KB/s，含 opus 累积（约 1/8 体积）+ 解码后 PCM |
| PSRAM：历史 | ≤32KB | 超出裁剪（§4.4） |
| PSRAM：单次 LLM 响应 | ≤8KB | 超出中止（§4.4） |
| 任务栈 | 8KB | DeviceAgent task |
| 队列 | 8×事件 | kOpusPacket 传指针不拷贝 payload |

防御规则：任何 PSRAM 分配失败 → 中止当前回合，提示音 + `SetStatus("内存不足")` → Idle，**绝不静默降级**。

## 7. 错误处理矩阵

| 阶段 | 错误 | 表现 | 恢复 |
|---|---|---|---|
| 唤醒/录音 | — | — | 无 |
| ASR | 超时/网络 | 提示音 + 状态栏 | 回 Listening 重说 |
| ASR | HTTP 4xx（key 错等） | 状态栏显示错误码 | 回 Idle，等下次唤醒重试 |
| LLM 单轮 | 超时/网络，重试 1 次后仍失败 | 状态栏 + 上屏"网络异常，请稍后再试" | 回 Idle，user 消息保留在历史（可追问） |
| LLM | 响应超 8KB/解析失败 | 同上 | 回 Idle |
| loop | 迭代达 8 轮 | 上屏固定兜底话术 | 正常收尾回 Idle |
| 工具 | 未知工具/参数非法/执行异常 | 结果文本回喂模型（模型可自纠错或向用户解释） | 不终止 loop |
| 打断 | loop 中唤醒/按键 | 停止状态栏动画 | 回滚 user 消息，回 Listening |
| 全局 | PSRAM 分配失败 | 提示音 + 状态栏 | 回 Idle |

副作用说明：工具副作用（如音量已改）在打断/失败时不回滚——与人类操作直觉一致，且回复未上屏时用户未知晓状态变化的风险由工具结果上屏缓解（kToolCall 事件已显示）。

## 8. 测试方案

### 8.1 宿主端单元测试（`test/host/`，独立 CMake，PC 上运行）

被测对象只依赖 cJSON 与标准库（`llm_transport.h`/`tool_registry`/`agent_loop`/`agent_context`/`agent_types`），设备无关。用例：

1. **loop 状态机**：mock transport 回放脚本——①纯文本→1 轮结束；②先 tool_call 后文本→2 轮；③连续 8 次工具调用→兜底；④transport 报错→kError；⑤cancel 置位→kAborted 且 user 消息回滚。
2. **历史裁剪**：构造超 32KB 历史 → 断言裁剪按完整轮次、system 保留、无悬空 tool_call_id。
3. **序列化**：`SerializeRequest()` 输出与 §4.4 规则快照比对（含 assistant 带 tool_calls、tool 消息回填）。
4. **ToolRegistry::Execute**：未知工具（`E_UNSUPPORTED`）/ arguments 非法 JSON（`E_INVALID_ARG`）/ 回调抛异常 / 正常返回信封，各结果文本快照比对；参数超界（如 `linear_mm_s=500`）被 Schema 限幅拒绝；`command_id` 幂等。
5. **WAV 头构造**（`asr_client` 中抽出纯函数 `BuildWavHeader`）：44 字节逐字段断言。

运行：`cmake -S test/host -B test/host/build && cmake --build ... && ctest`；CI 可选接入。

### 8.2 设备端分阶段验证

| 阶段 | 手段 | 通过标准 |
|---|---|---|
| M1 | 串口 `agent test "..."` | loop 日志可见轮次/工具执行/最终回复；打断命令生效 |
| M2 | 真实语音 | 说话→屏幕出 ASR 文本→回复；VAD 尾点自然 |
| M3 | loop 中再次唤醒 | 在途请求中止，无内存泄漏（`heap_caps` 前后对比），回 Listening |
| 长稳 | 连续 50 回合 | 无重启、PSRAM 峰值稳定、历史裁剪生效 |

## 9. 里程碑与验收标准

| 里程碑 | 内容 | 验收标准（DoD） |
|---|---|---|
| **M1** | `agent_types/context/tool_registry/llm_transport/llm_openai/agent_loop` + 串口 `agent test` 命令 + ToolRegistry 共享重构 + 宿主端单测 | 串口输入"把音量调到50"→设备实际改音量→串口返回确认文本；宿主测试全绿；官方服务器模式烧录回归正常 |
| **M2** | `asr_client` + DeviceAgent 编排 + 状态机/Display 接入 + Kconfig 分流 | 语音提问→屏幕出 ASR 与回复；全程无串口参与 |
| **M3** | 打断、错误矩阵全项、资源防御 | §8.2 M3 行通过；错误矩阵逐项人工触发通过 |
| **M4** | 串口配置命令完善、Mibot 机器人工具集（§4.5.1，需 Mibot 板卡硬件：`main/boards/mibot/`）、拍照/视觉工具、`kDeviceStateThinking` 动效 | `robot.move/stop/set_arm_pose` 经 loop 实际调用且限幅/TTL/急停生效；新工具经 loop 实际调用成功；配置命令文档化 |

## 10. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| `esp_opus_dec` 独立实例与 AudioService 解码器并存 | 解码异常/内存翻倍 | 独立实例本就是设计要求；M2 首日先做"解码自测"（录 3s→解码→CRC 比对宿主端参考） |
| mbedTLS PSRAM 分配在部分 IDF 版本行为差异 | TLS 建连失败 | `sdkconfig.defaults` 改动单独提交便于回退；失败时退回内部分配（仅慢，不影响功能） |
| 每请求新建 TLS 的握手延迟 | 每轮 +300~500ms | 预算已含；§11 keep-alive 复用为 v2 首选优化 |
| 大 system prompt + 工具描述挤压历史 | 轮次变少 | 裁剪按字节预算；prompt 控制在 2KB 内并计入预算 |
| 上游（78/xiaozhi-esp32）演进冲突 | 合并成本 | 改动集中于新目录 `main/agent/`；触碰点仅 §3 列出的 7 处，多为追加式 |
| 供应商 API 形状差异（如 tool 消息字段名） | 个别 provider 400 | OpenAI 兼容层为事实标准；发现的差异做成 per-provider 的请求改写钩子（v2），v1 用兼容度好的供应商验证 |

## 11. 未来扩展（v1 架构已预留的位置）

1. **TTS 语音回复**：新增 `tts_client.cc`（文本分句→`/audio/speech`→opus 包）→ PCM 可经现有 Opus 编码器回推 `PushPacketToDecodeQueue` 播放；DeviceAgent 增加 Speaking 态与播放期打断。不动 loop。
2. **LLM 流式（SSE）**：`llm_openai.cc` 换流式实现，`LlmTransport` 接口不变；回复可边到边上屏。
3. **Realtime 语音模型**：新增一种 `LlmTransport` 变体（WS），loop 层协议化改造，属于较大独立立项。
4. **keep-alive 连接复用**：`llm_openai` 内缓存 client，消握手延迟。
5. **历史持久化**：AgentContext 落 NVS/littlefs 快照。
6. **远程 MCP 工具**：设备作为 MCP Client 接入外部 MCP 服务器（扩展点见 §4.5）——智能家居、搜索、邮件等能力直达设备端 agent，无需任何自有服务器。扩展点为 `RegisterRemoteMcpTools(registry, endpoint)`，实现前接口已兼容。
7. **Skill 机制**：Skill = {system prompt 片段, 工具组, 可选远程 MCP 端点} 的配置化组合，存 assets/NVS；启用时 DeviceAgent 拼装——prompt 片段追加进 system（AgentContext 需支持多片段拼接，小改），工具组注册进 ToolRegistry（零改动），loop 不变。**边界**：ESP32 无法动态装载代码，Skill 的"可执行部分"只能以固件内置工具或远程 MCP 工具存在；设备上的 Skill 本质是"配置 + 提示词 + 工具清单"。
8. **免唤醒连续对话**：回复后保持 N 秒聆听窗口，VAD 断句后直接再入 loop。

---

## 附：改动文件全景

```
新增  main/agent/                    （8 个模块，见 §4.1）
新增  test/host/                     （宿主端测试）
新增  main/boards/mibot/             （M4 可选：机器人板卡，驱动 + 运动安全状态机 + robot.* 工具注册，见 §4.5.1）
修改  main/Kconfig.projbuild         （+1 menu）
修改  main/CMakeLists.txt            （SRCS 追加）
修改  main/application.cc            （2 处按开关分流，见 §3#3#4）
修改  main/device_state.h            （+1 枚举值）
修改  main/mcp_server.cc             （AddCommonTools 委托化，行为不变）
修改  sdkconfig.defaults             （+1 行 mbedTLS PSRAM）
```
