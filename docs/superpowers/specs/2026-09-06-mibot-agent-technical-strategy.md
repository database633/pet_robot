# Mibot Agent 体验层：技术策略与可行性/冲突分析

- 日期：2026-09-06
- 状态：**技术附录（草案 v0.1）**，配合《Agent 体验层设计》草案阅读；本文给机制细节、数字与冲突消解
- 上游：`2026-09-06-mibot-agent-experience-design.md`（方案 A：单内核双人格包）
- 注：SF32LB53 资源数字为知识区间（本机网络受限未能核实数据手册），落地前以实际模组确认，见 §7.1

---

## 1. 运行时架构（SF32 侧任务布局与事件总线）

### 1.1 任务布局（openvela/NuttX 线程模型）

| 任务 | 优先级 | 职责 | 与 ESP32 侧对照 |
|---|---|---|---|
| `reflex` | 高 | 事件总线消费、打断、需求衰减 tick、模式切换触发、KWS 关键词命中路由 | 对应"反应循环"；不碰安全权威 |
| `deliberative` | 中 | AgentLoop / 桌宠策略调用、技能执行、记忆读写 | 对应"慎思循环" |
| `audio_in` | 中 | 麦克风采播 + VAD + KWS 特征提取 | SF32 麦克风（《规范》§2.1 VAD 在 SF32） |
| `audio_out` | 中 | 音效包播放 / TTS 音频播放 | 本地播放；不走 ESP32 AUDIO_DOWN |
| `lcd` | 低 | 表情状态机渲染 | 表情真值来自执行结果（见 §8-C7） |
| `comm` | 低 | UART 帧收发（对接本仓库 M1 协议栈对端） | 《规范》§3 |

### 1.2 事件总线（reflex 的输入域）

```
priority 0：ESP32 EVENT(critical)          → 立即打断表演/语音
priority 1：用户注意（KWS 命中/模式词/唤醒）→ 打断当前表演
priority 2：需求阈值穿越（boredom>0.7 等）
priority 3：ToF 接近事件 / MCP 推送 / 定时习惯
```

- 队列有界（如 32 条），溢出丢弃同类型未处理事件（防风暴）
- **打断语义**：P0/P1 事件置 `interrupt_flag` + 取消在飞策略调用（cooperative cancel——deliberative 在每个步骤边界检查）；正在播的声音 fade-out ≤200ms；正在发的 robot.* 命令靠 ESP32 侧 TTL 自然截止（不发 stop 也安全，但主动 stop 更干净——发）

### 1.3 需求/情感数值模型（AffectEngine，单写者）

```
state: valence[-1,1], arousal[0,1], energy[0,1], bond[0,100],
       needs: {boredom, play_drive}   // 预留 hunger/battery-aware
更新源（全部汇入 AffectEngine，见 §8-C10 单写者原则）：
  规则 tick（1s，整数运算）：energy -0.5%/min 空闲；boredom +1%/min 无交互；
  交互增量：被搭话 +valence、玩耍 +play_drive 消耗、被冷落 -valence 小幅、
  收到 EVENT(edge_detected) → arousal 突增（吓一跳素材）
  LLM delta 提交：慎思循环在回合末输出 {valence:+0.2, note:"被夸了"} → 引擎校验幅度（|Δ|≤0.3）后应用
持久化：affect.json 每 60s 或事件驱动落盘；断电丢失 ≤60s 演化，可接受
```

### 1.4 桌宠策略调用（policy call）契约

```json
// 请求（system 约 250 tok + 状态块 ~150 tok + 近期事件 ~200 tok）
{"mode":"pet","state":{"v":0.6,"a":0.3,"e":0.8,"bond":42,"boredom":0.75},
 "recent":["09:12 有人靠近未互动","09:15 播放过无聊音效"],
 "body":{"motion_state":"STANDBY","tof_near":false,"battery_mv":3900},
 "vocabulary":["idle_sit","wander","beg_play","nap","chirp_attention","wiggle_happy"],
 "ask":"选择下一个行为或保持，输出 performance JSON"}
// 响应（强制 JSON，schema 同体验层设计 §7）
{"emote":"bored","sound":"whine_soft","motion":null,"lcd":"bored_01","say":null,"hold_s":180}
```

- 解析失败重试 1 次 → 仍失败执行**规则回退**（boredom>0.5 → wander；有 ToF near → attention 词）——策略层永远有底
- `hold_s`（保持秒数）允许 LLM 声明"接下来 N 秒不必再问我"，与 30s 最小间隔共同省调用

## 2. 记忆体系技术细节

### 2.1 存储布局（SF32 文件系统，NuttX LittleFS）

```
/pet/memory/
  episodic.jsonl      # 追加写；每行 {ts,kind,summary,tags[],importance,valence_delta,consolidated}
  semantic.json       # {facts:[{id,text,tags[],confidence,created,updated,source_ids[]}]}
  affect.json         # §1.3
  skills/*.json       # 程序性记忆（编译后技能包，§3.1）
  wm_high             # 固化水位（最后一条已固化 episodic 序号）
```

- **episodic 环形**：容量 500 行硬上限；淘汰 = 已固化 ∪ 最低 `importance × recency衰减`
- **重要性初值**（规则）：用户直接交互 0.6、错误/异常 0.5、环境事件 0.3、心跳/遥测 不记录
- **RAM 占用**：检索时流式扫描 episodic（不整载）；semantic 全量驻 RAM（200 条 × ~200B ≈ 40KB）——可控

### 2.2 固化（consolidation）流程

```
触发：空闲≥2h ∨ 每 50 条未固化 ∨ 夜间窗口（可配）
过程：
  1. 取 ≤30 条未固化 episodic（时间序）
  2. 一次快速模型调用 → {new_facts[], fact_updates[{id,confidence_delta,text?}], importance_adj[]}
  3. 合并规则：同 tags ∩ 文本相似(编辑距离/关键词重合) → 合并，confidence 新者权重高；
     矛盾 → updated 覆盖，旧文进 history 字段（保留 1 代）
  4. 置 consolidated 标记 + 推进水位；写失败 → 下个窗口重试（episodic 永不因固化失败丢失）
成本：1-2 次/天 × ~2K tokens（快速模型）≈ 可忽略
```

### 2.3 检索评分与注入预算

```
score(fact) = 0.4×tag_match(query_ctx) + 0.3×recency + 0.2×importance + 0.1×bond_relevance
Top-K=8 注入；预算表：
  助手模式 system：人格 300 + 技能索引 400 + 工具索引 300 + 语义 Top-K 400 + 工作上下文 800 ≈ 2.2K tok
  桌宠模式 policy：人格 250 + 词汇表 300 + 状态块 150 + 近期事件 200 ≈ 900 tok
（对齐 spec v1 的 8KB 工具预算同级纪律；超限先裁语义层，再裁技能索引）
```

### 2.4 隐私与完整性

- 记忆全部 SF32 本地；上行内容 = 派生事实（semantic 注入块），原始 episodic 不出设备
- "忘记我"分层擦除：semantic.json 清空 → episodic 截断 → affect 重置；每层语音确认
- 完整性：启动时 JSON 校验，坏行跳过并计数（>5% 触发重建索引）；episodic 追加写天然抗掉电

## 3. Skill 体系技术细节

### 3.1 技能包格式（源 YAML / 设备存编译后 JSON）

```json
{"name":"play_fetch","version":1,"type":"interactive",
 "summary":"扔球捡回","triggers":["扔球","ball_thrown"],
 "pre":{"motion_state":["STANDBY"],"energy":">0.4"},
 "steps":[{"say":null,"sound":"excited_bark","lcd":"alert_01"},
          {"tool":"robot.set_arm_pose","args":{"servo":2,"angle":120},"timeout_ms":2000},
          {"tool":"robot.move","args":{"linear_mm_s":80,"angular_deg_s":0,"duration_ms":1000},
           "safety":{"linear_mm_s":120,"angular_deg_s":90,"duration_ms":5000}}],
 "on_fail":{"sound":"whine","episodic":"fetch_failed"},
 "safety_class":"motion"}
```

- **编译**：PC 端工具 YAML→JSON + schema 校验（设备不做 YAML 解析）
- **安全钳位**：编译期把每步 args 与 safety 上限比对，超限拒载；motion 类技能的限幅≤用户命令限幅
- **索引预算**：system prompt 每技能 ~30 tok（名+一句话），上限 24 技能 ≈ 720 tok；超出 → 用户禁用/淘汰

### 3.2 学会新把戏（程序性记忆形成流程）

```
1. 触发：用户说"记住刚才" ∨ 启发式（同一非常规行为序列成功≥2 次）
2. LLM 读相关会话片段 → 输出技能草稿 JSON（§3.1 格式）
3. 校验链：schema → 工具白名单 → 安全钳位 → 命名去重
4. 用户确认（语音 + LCD 展示技能名）→ 落盘 skills/ + 注册表刷新
5. 配额：学习型 ≤10 个（flash 约束），超出提示淘汰；可列出/删除
```

### 3.3 执行引擎

- 顺序执行 steps；每步 timeout；任意步失败 → on_fail + episodic 记录 + 中断
- 每步边界检查 `interrupt_flag`（§1.2）→ 取消时也记 episodic（"玩到一半被叫走了"——宠物味儿的记忆）

## 4. MCP 技术细节（含传输路径决策——见 §8-C1 冲突）

### 4.1 生命周期与缓存

- 懒连接：首次调用某 server 才建连；schema 缓存 24h 刷新或手动；连接即健康检查
- 调用 = 异步操作：timeout 默认 10s；超时/失败 → 技能步失败路径（§3.3），**绝不阻塞 reflex**
- 每回合工具调用预算沿用《规范》第 4 章（≤32 工具/8KB schema）；每 server 工具数上限可配

### 4.2 安全分级（工具风险白名单）

| 级别 | 例 | 桌宠模式 | 助手模式 |
|---|---|---|---|
| safe | 查询/只读（天气、日历读、灯色温） | ✅ 可自主触发 | ✅ |
| action | 有外部副作用（开关灯、播放音乐） | ⚠️ 仅技能内且低频 | ✅ |
| hazard | 门锁/电器大功率/支付/删除 | ❌ 永不 | 🔒 二次确认（对齐《规范》安全思想） |

- 分级来源：MCP server 注册时由用户标级（默认 action），设备端强制执行
- **原则：自主性越高，权限越窄**——桌宠模式一律白名单（默认只放 safe），助手模式黑名单+hazard 确认

## 5. 模式切换状态机

```
状态：PET / AGENT（+ 各自的 speech/performing 子态）
转换表：
  PET→AGENT：语音"说人话" ∨ App 开关；守卫：cooldown 60s 过；动作：语音确认一次（开放问题④默认值）
  AGENT→PET：语音"安静" ∨ App ∨ 云断连 ∨ 定时段；无确认；播"安静"音效后换人格包
  双向：在飞工具调用/动作 → 取消语义（§1.2）；切换事件写 episodic
KWS 关键词表：{唤醒词, "说人话", "安静"} 恒激活（audio_in 任务，§1.1）
  ——SF32 本地 KWS 是**新增能力依赖**；若 openvela 侧无现成 KWS：
     降级 A：云端 ASR 关键词（延迟 1-2s，切换体验打折但可用）
     降级 B：非语音通道（ToF 敲桌节拍/串口命令/App）先行，语音切换后补
```

## 6. 成本与资源模型

### 6.1 云调用成本（按快速模型档 $0.5/M in、$2/M out 估）

| 调用类 | 频率（上限估） | 规模 | 日成本 |
|---|---|---|---|
| 桌宠 policy | 事件驱动 + ≥30s 间隔 + hold_s；峰值 ~200 次/天 | in 0.9K / out 0.1K | ~$0.13 |
| 记忆固化 | 1-2 次/天 | ~2K tok | ~$0.002 |
| 助手对话 | 用户驱动 | 现有设计 | 现有水平 |
| TTS（agent 模式） | 用户驱动 | 现有设计 | 现有水平 |

**结论：桌宠自主性云成本 ≈ 每天几美分量级**；`hold_s` + 事件驱动是主要杠杆。

### 6.2 SF32 存储

| 项 | 预算 | 说明 |
|---|---|---|
| 记忆四件套 | ≤2MB（episodic 0.5 + semantic 0.05 + skills 0.5 + 余量） | flash |
| 音效包 | 20 个 × opus ~20KB ≈ 0.4MB；wav 原始则 2-5MB | **必须 opus/压缩** |
| 表情帧 | 30 帧 × 10-30KB ≈ 0.3-0.9MB | 按分辨率 |
| KWS 模型（若本地） | 0.5-2MB | 依 openvela 侧方案 |

### 6.3 延迟体验

- 音效响应（本地包）：≤100ms——桌宠模式的"跟手"来自本地资源，LLM 只决定"何时演"
- 策略调用延迟：快速模型 0.3-1s，被 hold_s/缓存摊薄；对自主行为无感
- 模式切换：本地 KWS 即时；云端 ASR 路径 +1-2s

## 7. 可行性结论

### 7.1 SF32 硬件资源（**区间待核实**）

SF32LB53 为 Cortex-M55 级双核 + 外挂 PSRAM 的模组形态，openvela(NuttX) 带 LittleFS、opus 解码、LCD/音频框架。本设计需求（RAM <100KB 数据结构 + flash ≤6MB 含音效/表情/KWS）**大概率可行**；两个必须实测确认的点：① PSRAM/flash 实际容量与文件系统配额 ② 本地 KWS 模型是否放得下跑得动（M55 + 可用指令集加速则无忧）。

### 7.2 分维度结论

| 维度 | 结论 | 依据 |
|---|---|---|
| 云成本 | ✅ 可行 | §6.1，~$0.1/天 量级 |
| 存储 | ✅ 可行（含 opus 前提） | §6.2 |
| 实时性 | ✅ 可行 | 安全实时在 ESP32（已实现）；SF32 侧均为软实时 |
| 协议 | ✅ 可行，**带一个例外** | 桌宠自主性零新帧；**MCP 传输是唯一破口**（§8-C1） |
| SF32 能力依赖 | ⚠️ 两个新增依赖 | 本地 KWS（或降级方案）；文件系统配额 |
| 工程量 | 中 | P0-P3 分期（体验层设计 §9），P0 无硬件依赖可先行 |

## 8. 冲突清单与消解（C1-C12）

**C1｜MCP 传输 vs "ESP32 唯一网络出口"（最重大）**
冲突：MCP server 是网络服务；架构里 Wi-Fi 与云凭据都在 ESP32（spec v2 安全模型），SF32 若自开网络则绕过网关安全边界与单连接约束，若不开则 MCP 调用无路可走。
消解（三选一）：
  a) **网关泛化（推荐）**：ESP32 增加通用 HTTPS 代理帧（新 TYPE 0x42 GENERIC_HTTP 或 AI_REQUEST 增 `kind:"http"`），URL 白名单存 ESP32 NVS，仅放行已注册 MCP server——保持单网络点 + 凭据不出 ESP32；代价：本仓库 M4+ 新增一帧 + 代理代码（约一天量级），**打破"零新帧"承诺但只是加类型不改旧契约**
  b) SF32 开自己的 Wi-Fi（若模组硬件支持）：MCP 直连；代价：双 WiFi 栈/功耗/安全模型分裂，与《规范》§7 精神冲突
  c) MCP 缓行：先只做本地工具 + 云端 LLM 工具（现有 AI 帧即可），MCP 等 a) 就位
遗留风险：a) 的白名单管理需要配置命令（`mibot mcp add <url>` 一类，进 console）。

**C2｜宠物自主动作 vs 1500ms 保活租约（《规范》§3.5）**
冲突：策略最小间隔 30s ≫ 租约 1500ms——宠物自发 move 后若 SF32 不续租，1.5s 即被 ESP32 本地刹车。
消解：SF32 **动作包装器（Motion Wrapper）**：任何来源（用户命令/宠物行为/技能步）的 robot.* 运动命令统一由它发起并自动维护租约直到完成/超时——宠物不感知此细节。设计上与既有契约零冲突（租约本来就是 SF32 的义务），但必须写成强制规范，否则宠物"走两步就趴窝"。
遗留风险：包装器 bug 导致租约饥饿 → 表现为宠物动作频繁刹车（易发现易定位）。

**C3｜自主动作 vs E_BUSY/安全拒绝的反馈环**
冲突：宠物行为被 ESP32 拒绝（E_BUSY/边缘刹车）后，若策略层不知情会重试 → 死循环。
消解：行为选择器输入必须含"最近拒绝事件"；拒绝后 10s 冷却同类行为；BRAKING 期间只允许选择非运动行为。

**C4｜表情真值 vs LLM 表演意图（《规范》§8.2 步骤 5）**
冲突：performance 指令里的 `lcd` 是"意图"；若直接渲染，执行失败时表情说谎。
消解：LCD 表情状态机只订阅**执行结果**（ACK/EVENT/本地播放状态）；LLM 的 lcd 字段降级为"请求"，由状态机按真值仲裁（例：表演指令的开心表情在实际被拒后显示困惑）。

**C5｜affect 双写**
冲突：规则 tick 与 LLM delta 同时改心情 → 丢更新/竞态。
消解：单写者原则——AffectEngine 独占状态（reflex 任务内），LLM 只提交幅度受限的 delta 请求（§1.3），经队列串行应用。

**C6｜MCP 副作用 × 自主性**
冲突：桌宠自主触发外部世界操作（灯/电器），无人确认。
消解：§4.2 白名单分级——桌宠模式默认只放 safe 级；action 级仅限用户显式写进技能；hazard 级双模式皆需确认。自主 + 危险 = 构造性不可能。

**C7｜音效自播 vs 语音误触发**
冲突：宠物叫声经扬声器→麦克风回环，可能触发本地 KWS/云端 ASR。
消解：播放期间抑制 KWS（self-voice 门控）；音效包选择避开与关键词同频段特征（经验性，实测调）。

**C8｜ToF 接近事件风暴**
冲突：阈值附近抖动 → EVENT 刷爆 UART 与事件总线。
消解：ESP32 侧去抖（进入/离开各 500ms 稳定）+ 同类型事件最小间隔 2s + 迟滞阈值（进 150mm/出 200mm）；SF32 侧总线再兜底丢弃（§1.2）。

**C9｜固化调用 vs 云端单连接**
冲突：spec v2"同时刻仅一条云端连接"，固化若与用户对话并发则排队/互斥。
消解：固化只在空闲触发（设计使然），且走 ESP32 网关同队列串行——天然错峰，无协议改动。

**C10｜模式切换与在飞操作的竞态**
冲突：切换瞬间有未完成工具调用/动作。
消解：§1.2 取消语义 + ESP32 TTL 兜底；切换写 episodic 保证记忆连续性（"我们说到一半切回宠物了"下次可续）。

**C11｜与里程碑的排期冲突**
冲突：本设计 P2/P3 依赖 robot.* 执行（M3）与 ToF（M2），MCP 依赖 C1 决策（可能动 ESP32）。
消解：P0/P1 无硬件依赖可与 M2/M3 并行；MCP 缓行选项（C1-c）解除排期耦合；若采纳 C1-a，ESP32 增量一帧放进 M4 计划，不影响 M1-M3。

**C12｜记忆固化错误事实污染**
冲突：LLM 固化产出错误"事实"并长期注入 prompt。
消解：固化输出带 confidence，低置信不注入；矛盾保留 history 一代可回滚；用户可查看/编辑记忆（console 命令 `pet.mem list/fix`）；每条 fact 带 source_ids 可溯源到 episodic。

## 9. 对既有计划的修改建议

| 文档 | 修改 |
|---|---|
| M2 计划（待写） | 若开放问题⑤通过：ToF 任务加 proximity_changed EVENT（去抖+迟滞，§8-C8 参数） |
| M4 计划 | 若 C1-a 采纳：加 GENERIC_HTTP 代理帧（TYPE 0x42）+ URL 白名单 + console 配置命令 |
| 《规范》符合性 | 全部冲突消解均为 SF32 侧实现纪律或 ESP32 加帧，不改《规范》既有契约语义 |

## 10. 决策点汇总（请在评审时拍板）

1. **C1 传输路径**：a 网关泛化（推荐）/ b SF32 独立网络 / c MCP 缓行
2. 开放问题①-⑤（体验层设计 §10）中与本策略强相关：③ 记忆预算 1-2MB 是否接受；⑤ ToF 接近事件进 M2
3. 新增确认：本地 KWS 是否为 SF32 侧既有能力（决定模式切换体验档位）；音效包格式 opus 是否可接受
