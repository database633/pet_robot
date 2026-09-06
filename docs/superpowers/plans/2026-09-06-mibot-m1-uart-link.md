# Mibot M1：UART 链路栈 + 板卡骨架 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 Mibot ESP32-S3 的 M1 里程碑——完整 UART 链路栈（帧编解码/CRC/分片/HELLO/PING/TELEMETRY）+ 板卡骨架 + 串口配置命令 + PC 测试对端，达到 spec §8.2 验收步骤 1。

**Architecture:** 纯 C++ 的帧编解码层（`mibot_frame_codec`，无 ESP-IDF 依赖，可在 PC 上单元测试）+ 设备侧收发任务（`MibotUartLink`）+ 会话层（`MibotLinkService`，HELLO/HELLO_ACK、PING/PONG、1Hz TELEMETRY）。板卡以 xiaozhi `WifiBoard` 为基类，扁平文件布局放进 `main/boards/mibot/`（板卡源文件由 `main/CMakeLists.txt:878` 的顶层 GLOB 收集，不新建子目录）。SF32 未就绪期间用 Python 测试对端（USB-UART 直连）做端到端验证。

**Tech Stack:** ESP-IDF（C++17/FreeRTOS/uart driver/cJSON/esp_console）、宿主测试用裸 CMake + 自带 30 行断言框架（零外部依赖）、PC 对端用 Python3 + pyserial。

**Spec:** `docs/superpowers/specs/2026-09-06-esp32-device-agent-design.md`（v2）§3、§4.1、§8.2 步骤 1、§9 M1。

**三个默认决策**（评审问题未获回复，按此执行，可随时推翻）：
1. `model` 等 LLM 配置完全由 ESP32 NVS 拥有，SF32 的 AI_REQUEST 不能指定（M4 计划落实）。
2. `set_expression` 误发到 ESP32 → NACK `E_UNSUPPORTED`。
3. 里程碑 = 《规范》§8 七步顺序，不做颗粒度调整。

**与 spec 的已知偏离**（M1 范围内的简化，M2 计划中消除）：
- RX 用 `uart_driver` 4096 字节环形缓冲 + 20ms 轮询读，不用 DMA（M1 无音频流，带宽无压力；M2 引入 AUDIO_UP 时评估切 DMA）。
- 控制/音频双队列拆分推迟到 M2（M1 仅 TX 队列）。
- 保活租约 1500ms → `BRAKING` 属 M2（依赖运动状态机）。

**对抗性评审产生的前向约束（M2 计划必须落实，M1 不做）**：
- RX 任务上下文只做"解码 + 分发"；M2 的 COMMAND 执行（含最长 5s 的工具执行）必须投递到独立执行任务，否则会阻塞 HELLO/PONG/TELEMETRY。
- `Send()` 每帧一次堆分配（`new Frame`）；M2 引入 50Hz 音频帧后改内存池/双队列。
- M1 不提供链路 Stop/重启（与板卡同生命周期）；M2 若需错误恢复，按 `running_` 标志 + 任务自退出方式补。
- **首次配网入口缺失**：M1 板卡无按键无屏幕，NVS 无 Wi-Fi 凭据时无法进入/完成配网（xiaozhi 配网靠板卡按钮触发）。M1 验证用预烧录凭据（`idf.py monitor` 下临时方案或提前 `nvs` 写入）；M2 必须补配网通道（UART 命令触发配网或保留 BOOT 键）。

---

## 文件结构

| 文件 | 职责 | 宿主可测 |
|---|---|---|
| `main/boards/mibot/mibot_frame_codec.h/.cc` | 帧常量/结构体、CRC-16/CCITT-FALSE、EncodeFrame、FrameDecoder（字节流→帧）、FragmentReassembler | ✅（禁含 ESP-IDF 头） |
| `main/boards/mibot/mibot_config.h` | UART 端口/引脚/波特率、音频采样率宏、固件版本 | — |
| `main/boards/mibot/mibot_uart_link.h/.cc` | 设备侧链路：RX/TX FreeRTOS 任务、发送队列 | ❌ |
| `main/boards/mibot/mibot_link_service.h/.cc` | 会话层：HELLO/HELLO_ACK、PING/PONG、1Hz TELEMETRY、未知帧 NACK | ❌ |
| `main/boards/mibot/mibot_console.h/.cc` | `mibot show` 串口诊断命令 + REPL | ❌ |
| `main/boards/mibot/mibot_board.cc` | `MibotBoard : WifiBoard` 骨架 + DECLARE_BOARD | ❌ |
| `main/boards/mibot/config.json` | 板卡发布变体描述 | — |
| `main/Kconfig.projbuild`（修改） | 新增 `BOARD_TYPE_MIBOT_ESP32S3` choice 条目 | — |
| `main/CMakeLists.txt`（修改） | 新增 `elseif(CONFIG_BOARD_TYPE_MIBOT_ESP32S3) set(BOARD_DIR "mibot")` | — |
| `test/host/test_framework.h`、`test/host/test_main.cpp`、`test/host/test_frame_codec.cpp`、`test/host/CMakeLists.txt`、`test/host/.gitignore` | 宿主单元测试 | — |
| `scripts/mibot_uart_peer.py` | PC 端 SF32 模拟对端（HELLO/PING/TELEMETRY 观察者） | — |

已核实的宿主事实（执行者不用再查）：
- `dummy_audio_codec.cc` 始终参与编译（`main/CMakeLists.txt:13`），include 路径 `"codecs/dummy_audio_codec.h"`（`audio/` 在 INCLUDE_DIRS，`main/CMakeLists.txt:46`）。
- `DummyAudioCodec::Read/Write` 恒返 0（`main/audio/codecs/dummy_audio_codec.cc:14`）；`audio_service.cc:308` 对读失败有 10ms 兜底延时，不会忙等。
- 板卡 GLOB 只收板卡目录顶层 `*.cc/*.c`（`main/CMakeLists.txt:878`）→ 文件扁平布局。
- 板卡采样率宏由各板自己的 config 头定义（如 `main/boards/bread-compact-esp32/config.h:6`）→ 放进 `mibot_config.h`。
- `GetNetworkStateIcon`/`SetPowerSaveLevel` 由 `WifiBoard` 提供默认实现（`main/boards/common/wifi_board.h:53-54`），M1 不覆写。

---

### Task 1: 宿主测试脚手架

**Files:**
- Create: `test/host/test_framework.h`
- Create: `test/host/test_main.cpp`
- Create: `test/host/CMakeLists.txt`
- Create: `test/host/.gitignore`
- Create: `main/boards/mibot/mibot_frame_codec.h`（空壳）
- Create: `main/boards/mibot/mibot_frame_codec.cc`（空壳）

- [ ] **Step 1: 写测试框架与入口**

`test/host/test_framework.h`：

```cpp
#pragma once
// 极简测试框架：无外部依赖，断言失败即退出非零
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

struct TestCase { std::string name; std::function<void()> fn; };
inline std::vector<TestCase>& GetTests() { static std::vector<TestCase> tests; return tests; }
inline int RegisterTest(const char* name, std::function<void()> fn) {
    GetTests().push_back({name, std::move(fn)});
    return 0;
}
#define MIBOT_TEST(name) \
    static const int reg_##name = RegisterTest(#name, name)
#define EXPECT_EQ(a, b) do { \
    if (!((a) == (b))) { \
        std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        std::exit(1); \
    } } while (0)
#define EXPECT_TRUE(x) EXPECT_EQ(static_cast<bool>(x), true)
```

`test/host/test_main.cpp`：

```cpp
#include "test_framework.h"

int main() {
    // 断言失败即 std::exit(1)，顺序执行即可；失败时最后一个 [RUN] 就是肇事用例
    for (auto& t : GetTests()) {
        std::printf("[ RUN  ] %s\n", t.name.c_str());
        t.fn();
        std::printf("[  OK  ] %s\n", t.name.c_str());
    }
    std::printf("%zu tests passed\n", GetTests().size());
    return 0;
}
```

`test/host/CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(mibot_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MIBOT_BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/../../main/boards/mibot)

add_executable(test_mibot
    test_main.cpp
    test_frame_codec.cpp
    ${MIBOT_BOARD_DIR}/mibot_frame_codec.cc
)
target_include_directories(test_mibot PRIVATE ${MIBOT_BOARD_DIR} ${CMAKE_CURRENT_LIST_DIR})

enable_testing()
add_test(NAME mibot COMMAND test_mibot)
```

`test/host/.gitignore`：

```
build/
```

- [ ] **Step 2: 写一个占位测试文件（此刻只含冒烟测试）**

`test/host/test_frame_codec.cpp`：

```cpp
#include "test_framework.h"

static void test_scaffold() { EXPECT_EQ(1, 1); }
MIBOT_TEST(test_scaffold)
```

- [ ] **Step 3: 建空实现让脚手架编译通过**

Create `main/boards/mibot/mibot_frame_codec.h`：

```cpp
#ifndef MIBOT_FRAME_CODEC_H
#define MIBOT_FRAME_CODEC_H
// Mibot UART 帧编解码。纯 C++，禁止包含任何 ESP-IDF 头（宿主单测直接编译本文件）。
// 帧格式：《Mibot 桌面机器人状态机与控制接口规范》§3.2
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mibot {
// Task 2-4 填充
}  // namespace mibot
#endif  // MIBOT_FRAME_CODEC_H
```

Create `main/boards/mibot/mibot_frame_codec.cc`：

```cpp
#include "mibot_frame_codec.h"
namespace mibot {
// Task 2-4 填充
}  // namespace mibot
```

- [ ] **Step 4: 构建并运行**

**规范入口（本机实际验证过）：**

```bash
"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe
```

Expected: `1 tests passed`，exit 0。（必须编译全部 3 个 TU：test_main、test_frame_codec、mibot_frame_codec。）

备选（有 cmake 的机器）：`cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`。

- [ ] **Step 5: Commit**

```bash
git add test/host main/boards/mibot/mibot_frame_codec.h main/boards/mibot/mibot_frame_codec.cc
git commit -m "mibot: host test scaffold for UART frame codec"
```

---

### Task 2: CRC-16/CCITT-FALSE（TDD）

**Files:**
- Modify: `test/host/test_frame_codec.cpp`
- Modify: `main/boards/mibot/mibot_frame_codec.h`
- Modify: `main/boards/mibot/mibot_frame_codec.cc`

- [ ] **Step 1: 写失败测试**

`test/host/test_frame_codec.cpp` 顶部（`#include "test_framework.h"` 之后）加：

```cpp
#include "mibot_frame_codec.h"

using namespace mibot;
```

末尾追加：

```cpp
// CRC-16/CCITT-FALSE 权威校验向量（poly 0x1021, init 0xFFFF, 不反射, 无异或输出）
static void test_crc_check_vector() {
    const char* s = "123456789";
    EXPECT_EQ(Crc16(reinterpret_cast<const uint8_t*>(s), 9), 0x29B1);
}
MIBOT_TEST(test_crc_check_vector)

static void test_crc_empty() {
    uint8_t dummy = 0;
    EXPECT_EQ(Crc16(&dummy, 0), 0xFFFF);  // 初值即空串结果
}
MIBOT_TEST(test_crc_empty)
```

- [ ] **Step 2: 运行确认失败**

Run: `"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe`
Expected: 编译失败——`Crc16` 未定义。

- [ ] **Step 3: 实现**

`mibot_frame_codec.h` 的 `namespace mibot` 内追加：

```cpp
// CRC-16/CCITT-FALSE（覆盖 VERSION..PAYLOAD）
uint16_t Crc16(const uint8_t* data, size_t len);
// 增量式：先算前段，再续算后段（帧头与 payload 分离存储时用）
uint16_t Crc16Update(uint16_t crc, const uint8_t* data, size_t len);
```

`mibot_frame_codec.cc` 追加：

```cpp
uint16_t Crc16Update(uint16_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

uint16_t Crc16(const uint8_t* data, size_t len) {
    return Crc16Update(0xFFFF, data, len);
}
```

- [ ] **Step 4: 运行确认通过**

Run: `"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe`
Expected: `3 tests passed`，ctest PASS。

- [ ] **Step 5: Commit**

```bash
git add test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.h main/boards/mibot/mibot_frame_codec.cc
git commit -m "mibot: CRC-16/CCITT-FALSE with check vector"
```

---

### Task 3: 帧编码与流式解码（TDD）

**Files:**
- Modify: `test/host/test_frame_codec.cpp`
- Modify: `main/boards/mibot/mibot_frame_codec.h`
- Modify: `main/boards/mibot/mibot_frame_codec.cc`

- [ ] **Step 1: 写失败测试**

追加到 `test/host/test_frame_codec.cpp` 末尾（帧长 = SOF(2) + VERSION(1) + TYPE(1) + FLAGS(1) + SEQ(2) + LENGTH(2) + PAYLOAD(n) + CRC(2) = `11 + n`）：

```cpp
static Frame MakeTestFrame(uint16_t seq, size_t payload_len) {
    Frame f;
    f.type = kFrameCommand;
    f.flags = FrameFlags::kAckRequest;
    f.seq = seq;
    for (size_t i = 0; i < payload_len; ++i) f.payload.push_back(static_cast<uint8_t>(i & 0xFF));
    return f;
}

static void test_encode_layout() {
    Frame f = MakeTestFrame(0x1234, 2);
    auto bytes = EncodeFrame(f);
    EXPECT_EQ(bytes.size(), 13u);  // 9B 头(SOF2+VER+TYPE+FLAGS+SEQ2+LEN2) + 2B payload + 2B CRC
    EXPECT_EQ(bytes[0], 0xAA);
    EXPECT_EQ(bytes[1], 0x55);
    EXPECT_EQ(bytes[2], kFrameVersion);
    EXPECT_EQ(bytes[3], kFrameCommand);
    EXPECT_EQ(bytes[4], FrameFlags::kAckRequest);
    EXPECT_EQ(bytes[5], 0x34);  // SEQ 低字节（小端）
    EXPECT_EQ(bytes[6], 0x12);
    EXPECT_EQ(bytes[7], 2);     // LEN 低字节
    EXPECT_EQ(bytes[8], 0);
}
MIBOT_TEST(test_encode_layout)

static void test_roundtrip() {
    Frame f = MakeTestFrame(0xBEEF, 300);
    auto bytes = EncodeFrame(f);
    FrameDecoder d;
    d.Feed(bytes.data(), bytes.size());
    Frame out;
    EXPECT_TRUE(d.PopFrame(out));
    EXPECT_EQ(out.type, kFrameCommand);
    EXPECT_EQ(out.flags, FrameFlags::kAckRequest);
    EXPECT_EQ(out.seq, 0xBEEF);
    EXPECT_TRUE(out.payload == f.payload);
    EXPECT_TRUE(!d.PopFrame(out));  // 没有更多帧
}
MIBOT_TEST(test_roundtrip)

static void test_two_frames_one_feed() {
    auto a = EncodeFrame(MakeTestFrame(1, 10));
    auto b = EncodeFrame(MakeTestFrame(2, 10));
    std::vector<uint8_t> all = a;
    all.insert(all.end(), b.begin(), b.end());
    FrameDecoder d;
    d.Feed(all.data(), all.size());
    Frame out;
    EXPECT_TRUE(d.PopFrame(out) && out.seq == 1);
    EXPECT_TRUE(d.PopFrame(out) && out.seq == 2);
}
MIBOT_TEST(test_two_frames_one_feed)

static void test_resync_on_garbage() {
    Frame f = MakeTestFrame(7, 4);
    auto bytes = EncodeFrame(f);
    std::vector<uint8_t> noisy = {0x00, 0xAA, 0x00, 0x55, 0xAA};  // 假 SOF 干扰
    noisy.insert(noisy.end(), bytes.begin(), bytes.end());
    FrameDecoder d;
    d.Feed(noisy.data(), noisy.size());
    Frame out;
    EXPECT_TRUE(d.PopFrame(out) && out.seq == 7);
}
MIBOT_TEST(test_resync_on_garbage)

static void test_bad_crc_dropped() {
    Frame f = MakeTestFrame(9, 4);
    auto bytes = EncodeFrame(f);
    bytes[bytes.size() - 1] ^= 0xFF;  // 破坏 CRC 高字节
    FrameDecoder d;
    d.Feed(bytes.data(), bytes.size());
    Frame out;
    EXPECT_TRUE(!d.PopFrame(out));
    EXPECT_EQ(d.error_count(), 1u);
}
MIBOT_TEST(test_bad_crc_dropped)

static void test_oversize_length_dropped() {
    std::vector<uint8_t> bytes = {0xAA, 0x55, 0x01, 0x10, 0x00, 0x00, 0x00, 0xFF, 0x7F};  // LEN=0x7FFF > 4096
    FrameDecoder d;
    d.Feed(bytes.data(), bytes.size());
    Frame out;
    EXPECT_TRUE(!d.PopFrame(out));
    EXPECT_EQ(d.error_count(), 1u);
}
MIBOT_TEST(test_oversize_length_dropped)
```

- [ ] **Step 2: 运行确认失败**

Run: `"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe`
Expected: 编译失败——`Frame`/`EncodeFrame`/`FrameDecoder` 未定义。

- [ ] **Step 3: 实现**

`mibot_frame_codec.h` 的 `namespace mibot` 内追加：

```cpp
constexpr uint8_t kFrameVersion = 0x01;
constexpr size_t kMaxPayload = 4096;  // 《规范》§3.2

enum FrameType : uint8_t {
    kFrameHello        = 0x01,
    kFrameHelloAck     = 0x02,
    kFrameCommand      = 0x10,
    kFrameAck          = 0x11,
    kFrameNack         = 0x12,
    kFrameEvent        = 0x20,
    kFrameTelemetry    = 0x21,
    kFrameAudioUp      = 0x30,
    kFrameAudioDown    = 0x31,
    kFrameAiRequest    = 0x40,
    kFrameAiResponse   = 0x41,
    kFrameCameraResult = 0x50,
    kFramePing         = 0x60,
    kFramePong         = 0x61,
};

struct FrameFlags {
    static constexpr uint8_t kAckRequest = 0x01;  // bit0 请求应答
    static constexpr uint8_t kAck        = 0x02;  // bit1 应答
    static constexpr uint8_t kError      = 0x04;  // bit2 错误
    static constexpr uint8_t kFragment   = 0x08;  // bit3 分片
};

struct Frame {
    FrameType type = kFramePing;
    uint8_t flags = 0;
    uint16_t seq = 0;
    std::vector<uint8_t> payload;
};

// 编码完整帧：SOF(2) VER(1) TYPE(1) FLAGS(1) SEQ(2,LE) LEN(2,LE) PAYLOAD(n) CRC(2,LE)
// CRC 覆盖 VERSION..PAYLOAD，小端存放
std::vector<uint8_t> EncodeFrame(const Frame& frame);

// 字节流解码器：Feed 任意长度字节；完整帧入内部队列，PopFrame 逐个取出。
// 丢失 SOF → 丢弃至下一个 SOF；VERSION 不符 / LEN > kMaxPayload / CRC 错 → 丢帧并 error_count++
class FrameDecoder {
public:
    void Feed(const uint8_t* data, size_t len);
    bool PopFrame(Frame& out);
    uint32_t error_count() const { return error_count_; }
    void Reset();

private:
    enum State { kSync0, kSync1, kHeader, kPayload, kCrc0, kCrc1 };
    State state_ = kSync0;
    std::vector<uint8_t> header_;   // 7 字节：VERSION,TYPE,FLAGS,SEQ_LO,SEQ_HI,LEN_LO,LEN_HI
    std::vector<uint8_t> payload_;
    uint16_t crc_rx_ = 0;
    uint32_t error_count_ = 0;
    std::vector<Frame> frames_;     // 用 vector + 取出索引，避免 deque 依赖
    size_t pop_index_ = 0;
};
```

`mibot_frame_codec.cc` 追加：

```cpp
namespace {
constexpr uint8_t kSof0 = 0xAA;
constexpr uint8_t kSof1 = 0x55;
constexpr size_t kHeaderSize = 7;  // VERSION..LEN_HI（不含 SOF）
}  // namespace

std::vector<uint8_t> EncodeFrame(const Frame& frame) {
    std::vector<uint8_t> out;
    out.reserve(9 + frame.payload.size() + 2);
    out.push_back(kSof0);
    out.push_back(kSof1);
    out.push_back(kFrameVersion);
    out.push_back(static_cast<uint8_t>(frame.type));
    out.push_back(frame.flags);
    out.push_back(static_cast<uint8_t>(frame.seq & 0xFF));
    out.push_back(static_cast<uint8_t>((frame.seq >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(frame.payload.size() & 0xFF));
    out.push_back(static_cast<uint8_t>((frame.payload.size() >> 8) & 0xFF));
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    uint16_t crc = Crc16(out.data() + 2, out.size() - 2);  // VERSION..PAYLOAD
    out.push_back(static_cast<uint8_t>(crc & 0xFF));
    out.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    return out;
}

void FrameDecoder::Reset() {
    state_ = kSync0;
    header_.clear();
    payload_.clear();
    crc_rx_ = 0;
    frames_.clear();
    pop_index_ = 0;
}

void FrameDecoder::Feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        switch (state_) {
            case kSync0:
                if (byte == kSof0) state_ = kSync1;
                break;
            case kSync1:
                if (byte == kSof1) {
                    state_ = kHeader;
                    header_.clear();
                } else {
                    state_ = (byte == kSof0) ? kSync1 : kSync0;  // 0xAA 0xAA 0x55 也能同步
                }
                break;
            case kHeader:
                header_.push_back(byte);
                if (header_.size() == kHeaderSize) {
                    if (header_[0] != kFrameVersion) {
                        ++error_count_;
                        state_ = kSync0;
                        break;
                    }
                    size_t length = static_cast<size_t>(header_[5]) | (static_cast<size_t>(header_[6]) << 8);
                    if (length > kMaxPayload) {
                        ++error_count_;
                        state_ = kSync0;
                        break;
                    }
                    payload_.clear();
                    payload_.reserve(length);
                    state_ = (length == 0) ? kCrc0 : kPayload;
                }
                break;
            case kPayload:
                payload_.push_back(byte);
                if (payload_.size() ==
                    (static_cast<size_t>(header_[5]) | (static_cast<size_t>(header_[6]) << 8))) {
                    state_ = kCrc0;
                }
                break;
            case kCrc0:
                crc_rx_ = byte;
                state_ = kCrc1;
                break;
            case kCrc1: {
                crc_rx_ |= static_cast<uint16_t>(byte) << 8;
                uint16_t expected = Crc16Update(Crc16Update(0xFFFF, header_.data(), header_.size()),
                                                payload_.data(), payload_.size());
                if (expected == crc_rx_) {
                    Frame f;
                    f.type = static_cast<FrameType>(header_[1]);
                    f.flags = header_[2];
                    f.seq = static_cast<uint16_t>(header_[3]) | (static_cast<uint16_t>(header_[4]) << 8);
                    f.payload = std::move(payload_);
                    frames_.push_back(std::move(f));
                } else {
                    ++error_count_;
                }
                state_ = kSync0;
                break;
            }
        }
    }
}

bool FrameDecoder::PopFrame(Frame& out) {
    if (pop_index_ >= frames_.size()) {
        frames_.clear();
        pop_index_ = 0;
        return false;
    }
    out = std::move(frames_[pop_index_++]);
    return true;
}
```

- [ ] **Step 4: 运行确认通过**

Run: `"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe`
Expected: `9 tests passed`，ctest PASS。

- [ ] **Step 5: Commit**

```bash
git add test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.h main/boards/mibot/mibot_frame_codec.cc
git commit -m "mibot: frame encode/streaming decode with resync and error accounting"
```

---

### Task 4: 分片重组（TDD）

**Files:**
- Modify: `test/host/test_frame_codec.cpp`
- Modify: `main/boards/mibot/mibot_frame_codec.h`
- Modify: `main/boards/mibot/mibot_frame_codec.cc`

设计约定（spec §3.1，《规范》§3.2 只定义了 bit3=分片，此处补全语义）：分片帧 FLAGS 含 `kFragment`，**同 TYPE + 同 SEQ**，payload = `[total(1B), index(1B)] + data`；UART 保证有序到达，仅支持按序重组；**相邻分片静默 >500ms** 或 SEQ/TYPE 变化或 index 不连续 → 丢弃该链，从 index=0 的分片重新开始（注意：是静默间隔而非链总时长——64KB AI 应答在 921600bps 下要传约 0.7s，按链总时长算会被误杀）；重组上限 64KB（`kMaxAssembled`）。

- [ ] **Step 1: 写失败测试**

文件顶部 include 区补 `#include <algorithm>`（`std::min`），末尾追加：

```cpp
static std::vector<Frame> MakeFragments(uint16_t seq, const std::vector<uint8_t>& data, size_t chunk) {
    std::vector<Frame> frags;
    size_t total = (data.size() + chunk - 1) / chunk;
    for (size_t i = 0; i < total; ++i) {
        Frame f;
        f.type = kFrameAiRequest;
        f.flags = FrameFlags::kFragment;
        f.seq = seq;
        f.payload.push_back(static_cast<uint8_t>(total));
        f.payload.push_back(static_cast<uint8_t>(i));
        size_t begin = i * chunk;
        size_t end = std::min(begin + chunk, data.size());
        f.payload.insert(f.payload.end(), data.begin() + begin, data.begin() + end);
        frags.push_back(std::move(f));
    }
    return frags;
}

static void test_fragment_reassembly() {
    std::vector<uint8_t> big(10000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i & 0xFF);
    auto frags = MakeFragments(42, big, 1000);
    FragmentReassembler r;
    Frame out;
    for (size_t i = 0; i + 1 < frags.size(); ++i) {
        EXPECT_TRUE(!r.Feed(frags[i], i * 10, out));
    }
    EXPECT_TRUE(r.Feed(frags.back(), 1000, out));
    EXPECT_TRUE(out.payload == big);
    EXPECT_EQ(out.seq, 42);
    EXPECT_EQ(out.type, kFrameAiRequest);
    EXPECT_EQ(out.flags, 0);
}
MIBOT_TEST(test_fragment_reassembly)

static void test_passthrough_non_fragment() {
    FragmentReassembler r;
    Frame in = MakeTestFrame(5, 8);
    Frame out;
    EXPECT_TRUE(r.Feed(in, 0, out));
    EXPECT_TRUE(out.payload == in.payload);
}
MIBOT_TEST(test_passthrough_non_fragment)

static void test_fragment_inactivity_timeout() {
    auto frags = MakeFragments(1, std::vector<uint8_t>(3000, 0xAB), 1000);
    FragmentReassembler r;
    Frame out;
    EXPECT_TRUE(!r.Feed(frags[0], 0, out));     // 开始链（3 片），等 index=1
    EXPECT_TRUE(!r.Feed(frags[1], 400, out));   // 距上一片 400ms ≤ 500ms，继续
    EXPECT_TRUE(!r.Feed(frags[2], 1100, out));  // 静默 700ms > 500ms → 弃链；index=2 无法作新链起点 → 丢弃
    // 全链重发（时间上连续）→ 成功重组
    EXPECT_TRUE(!r.Feed(frags[0], 1200, out));
    EXPECT_TRUE(!r.Feed(frags[1], 1300, out));
    EXPECT_TRUE(r.Feed(frags[2], 1400, out));   // 集齐
    EXPECT_TRUE(out.payload.size() == 3000u);
    EXPECT_EQ(out.seq, 1);
    EXPECT_EQ(out.flags, 0);
}
MIBOT_TEST(test_fragment_inactivity_timeout)

static void test_fragment_long_chain_no_timeout() {
    // 20 片、每片间隔 100ms → 链总时长 1.9s 远超 500ms，但相邻静默均 ≤ 500ms，必须能重组
    std::vector<uint8_t> big(20000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i & 0xFF);
    auto frags = MakeFragments(7, big, 1000);
    FragmentReassembler r;
    Frame out;
    for (size_t i = 0; i < frags.size(); ++i) {
        bool done = r.Feed(frags[i], static_cast<uint32_t>(i * 100), out);
        if (i + 1 < frags.size()) EXPECT_TRUE(!done);
    }
    EXPECT_TRUE(out.payload == big);
}
MIBOT_TEST(test_fragment_long_chain_no_timeout)
```

- [ ] **Step 2: 运行确认失败**

Run: `"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe`
Expected: 编译失败——`FragmentReassembler` 未定义。

- [ ] **Step 3: 实现**

`mibot_frame_codec.h` 的 `FrameDecoder` 定义之后追加：

```cpp
// 分片重组器（语义见 spec §3.1）：
//  - 非分片帧原样通过（返回 true，assembled=输入帧）
//  - 分片帧：payload = [total(1B), index(1B)] + data；同 TYPE+SEQ；按序到达
//  - 相邻分片静默 >500ms / SEQ 或 TYPE 变化 / index 不连续 → 丢弃旧链；新链必须从 index=0 开始
class FragmentReassembler {
public:
    static constexpr uint32_t kFragmentTimeoutMs = 500;  // 相邻分片最大静默间隔
    static constexpr size_t kMaxAssembled = 64 * 1024;

    // 返回 true 表示 assembled 是完整帧（重组完成或非分片直通）
    bool Feed(const Frame& frag, uint32_t now_ms, Frame& assembled);
    void Reset();

private:
    bool active_ = false;
    uint16_t seq_ = 0;
    uint8_t type_ = 0;
    uint8_t total_ = 0;
    uint8_t received_ = 0;
    uint32_t last_rx_ms_ = 0;
    std::vector<uint8_t> data_;
};
```

`mibot_frame_codec.cc` 追加：

```cpp
bool FragmentReassembler::Feed(const Frame& frag, uint32_t now_ms, Frame& assembled) {
    if (!(frag.flags & FrameFlags::kFragment)) {
        assembled = frag;
        return true;
    }
    if (frag.payload.size() < 2) {
        Reset();
        return false;
    }
    const uint8_t total = frag.payload[0];
    const uint8_t index = frag.payload[1];
    if (total == 0 || index >= total) {
        Reset();
        return false;
    }
    if (active_ && now_ms - last_rx_ms_ > kFragmentTimeoutMs) {
        Reset();  // 静默超时（无符号减法，自然处理回绕）
    }
    if (active_ && (frag.seq != seq_ || frag.type != type_)) {
        Reset();  // 对端换了链
    }
    if (!active_) {
        if (index != 0) return false;  // 新链必须从第 0 片开始
        active_ = true;
        seq_ = frag.seq;
        type_ = static_cast<uint8_t>(frag.type);
        total_ = total;
        received_ = 0;
        data_.clear();
        size_t est = static_cast<size_t>(total) * frag.payload.size();
        data_.reserve(est > kMaxAssembled ? kMaxAssembled : est);  // 封顶，防畸形 total 撑爆内存
    }
    if (index != received_) {  // 只支持按序（UART 保证有序）
        Reset();
        return false;
    }
    data_.insert(data_.end(), frag.payload.begin() + 2, frag.payload.end());
    last_rx_ms_ = now_ms;
    ++received_;
    if (received_ == total_) {
        assembled.type = static_cast<FrameType>(type_);
        assembled.flags = 0;
        assembled.seq = seq_;
        assembled.payload = std::move(data_);
        Reset();
        return true;
    }
    return false;
}

void FragmentReassembler::Reset() {
    active_ = false;
    seq_ = 0;
    type_ = 0;
    total_ = 0;
    received_ = 0;
    last_rx_ms_ = 0;
    data_.clear();
}
```

- [ ] **Step 4: 运行确认通过**

Run: `"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe`
Expected: `13 tests passed`，ctest PASS。

- [ ] **Step 5: Commit**

```bash
git add test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.h main/boards/mibot/mibot_frame_codec.cc
git commit -m "mibot: in-order fragment reassembly with inactivity timeout and size cap"
```

---

### Task 5: 板卡骨架与注册（编译通过）

**Files:**
- Create: `main/boards/mibot/mibot_config.h`
- Create: `main/boards/mibot/mibot_board.cc`
- Create: `main/boards/mibot/config.json`
- Modify: `main/Kconfig.projbuild`（`choice BOARD_TYPE` 块内追加条目）
- Modify: `main/CMakeLists.txt`（板卡分支链追加 elseif）

- [ ] **Step 1: 写板卡配置头**

`main/boards/mibot/mibot_config.h`：

```cpp
#ifndef MIBOT_CONFIG_H
#define MIBOT_CONFIG_H

#include <driver/gpio.h>

// UART 连接 SF32（引脚以实际原理图为准，当前为占位默认值）
#define MIBOT_UART_NUM      1
#define MIBOT_UART_TX_GPIO  GPIO_NUM_17
#define MIBOT_UART_RX_GPIO  GPIO_NUM_18
#define MIBOT_UART_BAUD     921600  // 《规范》§3.1；不稳则降 460800

// 板卡音频采样率（xiaozhi 约定：每板 config 头自定义；DummyAudioCodec 恒返 0 字节）
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

#define MIBOT_FW_VERSION    "0.1.0"

#endif  // MIBOT_CONFIG_H
```

- [ ] **Step 2: 写板卡类**

`main/boards/mibot/mibot_board.cc`：

```cpp
#include <esp_log.h>

#include "wifi_board.h"
#include "codecs/dummy_audio_codec.h"

#include "mibot_config.h"

#define TAG "MibotBoard"

// Mibot ESP32-S3：AI 网关 + 运动安全执行器（M1 仅骨架）。
// 无本地麦克风/喇叭/屏幕：Display 用基类 NoDisplay，音频用 DummyAudioCodec
// （Read/Write 恒返 0；audio_service.cc:308 的 10ms 兜底延时保证不忙等）。
class MibotBoard : public WifiBoard {
private:
    DummyAudioCodec codec_{AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE};

public:
    MibotBoard() {
        ESP_LOGI(TAG, "Mibot ESP32-S3 board init, fw=%s", MIBOT_FW_VERSION);
    }

    ~MibotBoard() override = default;

    std::string GetBoardType() override { return "mibot-esp32s3"; }

    AudioCodec* GetAudioCodec() override { return &codec_; }

    // GetNetworkStateIcon/SetPowerSaveLevel 用 WifiBoard 默认实现；
    // M2（运动安全）再覆写 SetPowerSaveLevel 保持 Wi-Fi 常连接。
};

DECLARE_BOARD(MibotBoard)
```

- [ ] **Step 3: 注册 Kconfig 条目**

在 `main/Kconfig.projbuild` 的 `choice BOARD_TYPE` 块内（任一既有 `config BOARD_TYPE_*` 旁）追加：

```text
    config BOARD_TYPE_MIBOT_ESP32S3
        bool "Mibot ESP32-S3 (AI gateway + motion safety executor)"
        depends on IDF_TARGET_ESP32S3
```

- [ ] **Step 4: 注册 CMake 分支**

在 `main/CMakeLists.txt` 板卡分支链中（任意 `elseif(CONFIG_BOARD_TYPE_...)` 旁）追加：

```cmake
elseif(CONFIG_BOARD_TYPE_MIBOT_ESP32S3)
    set(BOARD_DIR "mibot")
```

- [ ] **Step 5: 写 config.json**

`main/boards/mibot/config.json`（`type` 只允许小写字母/数字/`.`/`-`，见 `main/CMakeLists.txt:872` 的校验）：

```json
{
    "type": "mibot-esp32s3",
    "target": "esp32s3",
    "builds": [
        {
            "name": "mibot-esp32s3",
            "sdkconfig_append": []
        }
    ]
}
```

- [ ] **Step 6: 构建验证**

Run: `idf.py set-target esp32s3 && idf.py menuconfig`（`Xiaozhi Assistant` → `Target Board` 选 `Mibot ESP32-S3`；若用 Octal PSRAM 模组在此确认 `SPIRAM` → `Octal`）然后 `idf.py build`
Expected: 编译链接成功，无 "The selected board does not define BOARD_DIR" fatal error。

- [ ] **Step 7: Commit**

```bash
git add main/boards/mibot main/Kconfig.projbuild main/CMakeLists.txt
git commit -m "mibot: board skeleton registered as BOARD_TYPE_MIBOT_ESP32S3"
```

---

### Task 6: MibotUartLink——设备侧收发任务

**Files:**
- Create: `main/boards/mibot/mibot_uart_link.h`
- Create: `main/boards/mibot/mibot_uart_link.cc`

- [ ] **Step 1: 写接口头**

`main/boards/mibot/mibot_uart_link.h`：

```cpp
#ifndef MIBOT_UART_LINK_H
#define MIBOT_UART_LINK_H

#include <atomic>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "mibot_frame_codec.h"

namespace mibot {

// 设备侧 UART 链路（M1）：
//  - RX 任务：轮询读字节 → FrameDecoder → 分片重组 → handler 回调（RX 任务上下文执行，含 cJSON，栈 6144）
//  - TX 任务：Send() 拷贝入队 → 编码写出；队列满直接丢弃（重传是上层职责）
// M1 不提供 Stop：链路与板卡同生命周期；M2 若需错误恢复再按 running_ 标志 + 任务自退出补。
class MibotUartLink {
public:
    using FrameHandler = std::function<void(const Frame&)>;

    void Start(int uart_num, int tx_gpio, int rx_gpio, int baud, FrameHandler handler);
    void Send(const Frame& frame);

private:
    void RxLoop();
    void TxLoop();
    static void RxTrampoline(void* arg) { static_cast<MibotUartLink*>(arg)->RxLoop(); }
    static void TxTrampoline(void* arg) { static_cast<MibotUartLink*>(arg)->TxLoop(); }

    int uart_num_ = -1;
    FrameHandler handler_;
    QueueHandle_t tx_queue_ = nullptr;
    FrameDecoder decoder_;
    FragmentReassembler frags_;
    std::atomic<bool> running_{false};
};

}  // namespace mibot
#endif
```

- [ ] **Step 2: 写实现**

`main/boards/mibot/mibot_uart_link.cc`：

```cpp
#include "mibot_uart_link.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/uart.h>

#include "mibot_config.h"

#define TAG "MibotUart"

namespace mibot {

void MibotUartLink::Start(int uart_num, int tx_gpio, int rx_gpio, int baud, FrameHandler handler) {
    uart_num_ = uart_num;
    handler_ = std::move(handler);

    uart_config_t cfg = {};
    cfg.baud_rate = baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install((uart_port_t)uart_num_, 4096, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config((uart_port_t)uart_num_, &cfg));
    ESP_ERROR_CHECK(uart_set_pin((uart_port_t)uart_num_, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    tx_queue_ = xQueueCreate(16, sizeof(Frame*));
    running_ = true;
    // RX 栈 6144：handler 回调里做 cJSON 解析/构建（RX 任务上下文）；TX 无重活 4096 够
    xTaskCreate(RxTrampoline, "mibot_rx", 6144, this, 5, nullptr);
    xTaskCreate(TxTrampoline, "mibot_tx", 4096, this, 5, nullptr);
    ESP_LOGI(TAG, "uart%d started @%d tx=%d rx=%d", uart_num_, baud, tx_gpio, rx_gpio);
}

void MibotUartLink::Send(const Frame& frame) {
    if (!running_ || !tx_queue_) return;
    auto* copy = new Frame(frame);
    if (xQueueSend(tx_queue_, &copy, 0) != pdTRUE) {
        delete copy;
        ESP_LOGW(TAG, "tx queue full, frame type=0x%02x dropped", frame.type);
    }
}

void MibotUartLink::RxLoop() {
    uint8_t buf[512];
    for (;;) {
        int len = uart_read_bytes((uart_port_t)uart_num_, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (len <= 0) continue;
        decoder_.Feed(buf, (size_t)len);
        Frame f;
        while (decoder_.PopFrame(f)) {
            if (f.flags & FrameFlags::kFragment) {
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                Frame assembled;
                if (frags_.Feed(f, now, assembled)) {
                    handler_(assembled);
                }
            } else {
                handler_(f);
            }
        }
    }
}

void MibotUartLink::TxLoop() {
    for (;;) {
        Frame* frame = nullptr;
        if (xQueueReceive(tx_queue_, &frame, pdMS_TO_TICKS(100)) == pdTRUE && frame) {
            auto bytes = EncodeFrame(*frame);
            delete frame;
            size_t written = 0;
            while (written < bytes.size()) {
                int n = uart_write_bytes((uart_port_t)uart_num_, bytes.data() + written, bytes.size() - written);
                if (n > 0) written += (size_t)n;
                else vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
    }
}

}  // namespace mibot
```

- [ ] **Step 3: 构建验证**

Run: `idf.py build`
Expected: 编译通过（尚未被 board 调用，GLOB 收集 `*.cc` 已编入固件）。

- [ ] **Step 4: Commit**

```bash
git add main/boards/mibot/mibot_uart_link.h main/boards/mibot/mibot_uart_link.cc
git commit -m "mibot: uart link rx/tx tasks with frame decoder and fragment reassembly"
```

---

### Task 7: MibotLinkService——HELLO/PING/TELEMETRY 会话层

**Files:**
- Create: `main/boards/mibot/mibot_link_service.h`
- Create: `main/boards/mibot/mibot_link_service.cc`
- Modify: `main/boards/mibot/mibot_board.cc`（接入 link service）

- [ ] **Step 1: 写会话层头**

`main/boards/mibot/mibot_link_service.h`：

```cpp
#ifndef MIBOT_LINK_SERVICE_H
#define MIBOT_LINK_SERVICE_H

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mibot_frame_codec.h"
#include "mibot_uart_link.h"

// 会话层（spec §3）：HELLO→HELLO_ACK、PING→PONG（seq 回写用于测 RTT）、
// 1Hz TELEMETRY、未支持帧→NACK(E_UNSUPPORTED)。M1 无真实遥测源：
// motion_state 固定 MOTION_INIT，tof/servo 为 null，battery_mv=0。
// 遥测用专用任务（vTaskDelayUntil），不用 esp_timer：esp_timer 回调跑在
// 共享的小栈任务里，cJSON 构建 + WiFi 查询有栈溢出风险。
class MibotLinkService {
public:
    void Start(mibot::MibotUartLink& link);
    bool IsReady() const { return ready_; }  // 收到过 HELLO 即 READY

private:
    void OnFrame(const mibot::Frame& frame);
    void SendTelemetry();
    void SendJson(mibot::FrameType type, uint8_t flags, uint16_t seq_reply, const std::string& json);
    static void TelemetryTaskEntry(void* arg);
    void TelemetryLoop();

    mibot::MibotUartLink* link_ = nullptr;
    uint16_t tx_seq_ = 0;
    std::atomic<bool> ready_{false};
};

#endif
```

- [ ] **Step 2: 写会话层实现**

`main/boards/mibot/mibot_link_service.cc`：

```cpp
#include "mibot_link_service.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <cJSON.h>

#include "mibot_config.h"

#define TAG "MibotLink"

using mibot::Frame;
using mibot::FrameFlags;
using mibot::FrameType;

void MibotLinkService::Start(mibot::MibotUartLink& link) {
    link_ = &link;
    link_->Start(MIBOT_UART_NUM, MIBOT_UART_TX_GPIO, MIBOT_UART_RX_GPIO, MIBOT_UART_BAUD,
                 [this](const Frame& f) { OnFrame(f); });

    // 专用遥测任务：独立栈，避免 esp_timer 共享任务的栈限制
    xTaskCreate(TelemetryTaskEntry, "mibot_tel", 4096, this, 4, nullptr);
}

void MibotLinkService::SendJson(FrameType type, uint8_t flags, uint16_t seq_reply, const std::string& json) {
    Frame f;
    f.type = type;
    f.flags = flags;
    f.seq = seq_reply;
    f.payload.assign(json.begin(), json.end());
    link_->Send(f);
}

void MibotLinkService::OnFrame(const Frame& frame) {
    switch (frame.type) {
        case mibot::kFrameHello: {
            // SF32 → ESP32：{"schema":"mibot.uart.v1","fw_version":"...","proto_version":1,"device_id":"..."}
            cJSON* hello = cJSON_ParseWithLength(reinterpret_cast<const char*>(frame.payload.data()),
                                                 frame.payload.size());
            const char* peer_fw = "?";
            if (hello) {
                cJSON* fw = cJSON_GetObjectItem(hello, "fw_version");
                if (cJSON_IsString(fw)) peer_fw = fw->valuestring;
            }
            ESP_LOGI(TAG, "HELLO from SF32, fw=%s seq=%u", peer_fw, frame.seq);
            if (hello) cJSON_Delete(hello);

            cJSON* ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "schema", "mibot.uart.v1");
            cJSON_AddNumberToObject(ack, "proto_version", 1);
            cJSON_AddNumberToObject(ack, "max_frame", 4096);
            cJSON_AddStringToObject(ack, "fw_version", MIBOT_FW_VERSION);
            cJSON* caps = cJSON_AddArrayToObject(ack, "capabilities");
            cJSON_AddItemToArray(caps, cJSON_CreateString("audio_relay"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("motion"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("ai_gateway"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("camera"));
            char* json = cJSON_PrintUnformatted(ack);
            std::string payload(json);
            cJSON_free(json);
            cJSON_Delete(ack);
            SendJson(mibot::kFrameHelloAck, FrameFlags::kAck, frame.seq, payload);
            ready_ = true;
            break;
        }
        case mibot::kFramePing:
            // PONG：seq 回写 = PING 的 seq，便于对端测 RTT
            SendJson(mibot::kFramePong, FrameFlags::kAck, frame.seq, "{}");
            break;
        default:
            // M1 尚无 COMMAND/AI 处理：按《规范》§5.3 回 E_UNSUPPORTED
            SendJson(mibot::kFrameNack, FrameFlags::kError, frame.seq,
                     std::string("{\"schema\":\"mibot.uart.v1\",\"error\":{\"code\":\"E_UNSUPPORTED\","
                                 "\"message\":\"type 0x") + std::to_string(frame.type) + " not supported in fw " +
                     MIBOT_FW_VERSION + "\"}}");
            break;
    }
}

void MibotLinkService::SendTelemetry() {
    double ts_ms = static_cast<double>(esp_timer_get_time()) / 1000.0;  // int64，避免 int32 回绕
    int rssi = 0;
    bool wifi_connected = false;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        wifi_connected = true;
        rssi = ap.rssi;
    }
    cJSON* tel = cJSON_CreateObject();
    cJSON_AddStringToObject(tel, "schema", "mibot.telemetry.v1");
    cJSON_AddNumberToObject(tel, "ts_ms", ts_ms);
    cJSON_AddStringToObject(tel, "behavior_state", "-");          // SF32 职责，ESP32 未知
    cJSON_AddStringToObject(tel, "motion_state", "MOTION_INIT");  // M2 接入运动状态机后更新
    cJSON_AddNumberToObject(tel, "battery_mv", 0);                // M3 接入 ADC 后更新
    cJSON_AddNullToObject(tel, "tof");
    cJSON* motor = cJSON_AddObjectToObject(tel, "motor");
    cJSON_AddStringToObject(motor, "left", "off");
    cJSON_AddStringToObject(motor, "right", "off");
    cJSON_AddNullToObject(tel, "servo");
    cJSON* wifi = cJSON_AddObjectToObject(tel, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_connected);
    cJSON_AddNumberToObject(wifi, "rssi_dbm", rssi);
    char* json = cJSON_PrintUnformatted(tel);
    std::string payload(json);
    cJSON_free(json);
    cJSON_Delete(tel);
    SendJson(mibot::kFrameTelemetry, 0, tx_seq_++, payload);
}

void MibotLinkService::TelemetryTaskEntry(void* arg) {
    static_cast<MibotLinkService*>(arg)->TelemetryLoop();
}

void MibotLinkService::TelemetryLoop() {
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        SendTelemetry();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));  // 1Hz，不漂移
    }
}
```

- [ ] **Step 3: 接入板卡**

`mibot_board.cc` 三处修改：

include 区追加：

```cpp
#include "mibot_uart_link.h"
#include "mibot_link_service.h"
```

private 成员区（`codec_` 之后）追加：

```cpp
    mibot::MibotUartLink uart_link_;
    MibotLinkService link_service_;
```

构造函数体改为：

```cpp
    MibotBoard() {
        ESP_LOGI(TAG, "Mibot ESP32-S3 board init, fw=%s", MIBOT_FW_VERSION);
        link_service_.Start(uart_link_);  // M1：链路服务随板卡启动（不依赖 Wi-Fi）
    }
```

- [ ] **Step 4: 构建验证**

Run: `idf.py build`
Expected: 编译通过。

- [ ] **Step 5: Commit**

```bash
git add main/boards/mibot/mibot_link_service.h main/boards/mibot/mibot_link_service.cc main/boards/mibot/mibot_board.cc
git commit -m "mibot: link service with HELLO/PING/TELEMETRY session layer"
```

---

### Task 8: 串口 console 诊断命令

**Files:**
- Create: `main/boards/mibot/mibot_console.h`
- Create: `main/boards/mibot/mibot_console.cc`
- Modify: `main/boards/mibot/mibot_board.cc`（启动 REPL + 暴露 link service）

- [ ] **Step 1: 写命令模块**

`main/boards/mibot/mibot_console.h`：

```cpp
#ifndef MIBOT_CONSOLE_H
#define MIBOT_CONSOLE_H

class MibotLinkService;  // 全局类，定义在 mibot_link_service.h

namespace mibot {
// 非阻塞启动 esp_console REPL（UART0，自建任务）
void StartConsoleTaskAsync();
}  // namespace mibot

// 由 mibot_board.cc 实现：返回板卡持有的链路服务（启动早期可能为 nullptr）
MibotLinkService* MibotGetLinkService();

#endif
```

`main/boards/mibot/mibot_console.cc`：

```cpp
#include "mibot_console.h"

#include <cstring>

#include <esp_console.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/uart.h>

#include "mibot_config.h"
#include "mibot_link_service.h"

#define TAG "MibotConsole"

namespace mibot {

static int CmdMibot(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "show") != 0) {
        printf("usage: mibot show\n");
        return 1;
    }
    MibotLinkService* svc = MibotGetLinkService();
    printf("fw=%s uart=%d baud=%d tx=%d rx=%d ready=%s\n",
           MIBOT_FW_VERSION, MIBOT_UART_NUM, MIBOT_UART_BAUD,
           MIBOT_UART_TX_GPIO, MIBOT_UART_RX_GPIO,
           (svc && svc->IsReady()) ? "yes" : "no");
    return 0;
}

void StartConsoleTask() {
    // REPL 引导同款做法见 main/boards/sensecap-watcher/sensecap_watcher.cc:436
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.max_cmdline_length = 512;
    repl_config.prompt = "mibot> ";
    esp_console_register_help_command();

    esp_console_cmd_t cmd = {};  // 成员逐个赋值，规避 IDF 版本间结构体字段顺序差异
    cmd.command = "mibot";
    cmd.help = "Mibot board diagnostics";
    cmd.hint = "show";
    cmd.func = &CmdMibot;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

    esp_console_repl_t* repl = nullptr;
    esp_console_dev_uart_config_t uart_cfg = {};  // UART0 = 烧录/日志口
    uart_cfg.baud_rate = 115200;
    uart_cfg.tx_gpio = GPIO_NUM_43;
    uart_cfg.rx_gpio = GPIO_NUM_44;
    uart_cfg.port = UART_NUM_0;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

static void ConsoleTaskEntry(void*) {
    StartConsoleTask();
    vTaskDelete(nullptr);
}

void StartConsoleTaskAsync() {
    xTaskCreate(ConsoleTaskEntry, "mibot_cli", 6144, nullptr, 3, nullptr);
}

}  // namespace mibot
```

- [ ] **Step 2: 板卡接入 console**

`mibot_board.cc` 三处修改：

include 区追加：

```cpp
#include "mibot_console.h"
```

`#define TAG "MibotBoard"` 之后追加：

```cpp
static MibotLinkService* g_mibot_link_service = nullptr;
MibotLinkService* MibotGetLinkService() { return g_mibot_link_service; }
```

构造函数体改为：

```cpp
    MibotBoard() {
        ESP_LOGI(TAG, "Mibot ESP32-S3 board init, fw=%s", MIBOT_FW_VERSION);
        g_mibot_link_service = &link_service_;
        link_service_.Start(uart_link_);
        mibot::StartConsoleTaskAsync();
    }
```

- [ ] **Step 3: 构建验证**

Run: `idf.py build`
Expected: 编译通过。若 `esp_console_dev_uart_config_t`/`esp_console_cmd_t` 字段报错，以所装 IDF 的 `esp_console.h` 实际字段为准修正成员赋值（仅字段名层面）。

- [ ] **Step 4: Commit**

```bash
git add main/boards/mibot/mibot_console.h main/boards/mibot/mibot_console.cc main/boards/mibot/mibot_board.cc
git commit -m "mibot: console diagnostics (mibot show)"
```

---

### Task 9: PC 测试对端（SF32 模拟器）

**Files:**
- Create: `scripts/mibot_uart_peer.py`

- [ ] **Step 1: 写对端脚本**

`scripts/mibot_uart_peer.py`：

```python
#!/usr/bin/env python3
"""Mibot UART 测试对端：模拟 SF32 与 ESP32 固件对话。

用法：
  python mibot_uart_peer.py COM5 hello          # 发 HELLO，等待 HELLO_ACK
  python mibot_uart_peer.py COM5 ping 10        # 发 10 帧 PING，统计 RTT
  python mibot_uart_peer.py COM5 telemetry 20   # 观察 20 秒 TELEMETRY
需要: pip install pyserial
"""
import json
import struct
import sys
import time

import serial

SOF = b"\xAA\x55"
VERSION = 0x01

TYPE_HELLO, TYPE_HELLO_ACK = 0x01, 0x02
TYPE_TELEMETRY = 0x21
TYPE_PING, TYPE_PONG = 0x60, 0x61

FLAG_ACK_REQUEST, FLAG_ACK, FLAG_ERROR, FLAG_FRAGMENT = 0x01, 0x02, 0x04, 0x08


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, flags: int, seq: int, payload: bytes) -> bytes:
    body = struct.pack("<BBBHH", VERSION, ftype, flags, seq, len(payload)) + payload
    return SOF + body + struct.pack("<H", crc16_ccitt_false(body))


class Decoder:
    """增量解码：feed(新字节) → 返回本次解出的 (type, flags, seq, payload) 列表"""

    def __init__(self):
        self.buf = b""

    def feed(self, data: bytes):
        self.buf += data
        frames = []
        while True:
            start = self.buf.find(SOF)
            if start < 0:
                self.buf = b""
                break
            self.buf = self.buf[start:]
            if len(self.buf) < 9:  # SOF(2) + 头(7)
                break
            ver, ftype, flags, seq, length = struct.unpack("<BBBHH", self.buf[2:9])
            if ver != VERSION or length > 4096:
                self.buf = self.buf[1:]  # 假 SOF / 坏头：滑动 1 字节重新同步
                continue
            frame_len = 11 + length  # SOF(2) + 头(7) + payload + CRC(2)
            if len(self.buf) < frame_len:
                break  # 半帧，等更多数据
            payload = self.buf[9:9 + length]
            (crc,) = struct.unpack("<H", self.buf[9 + length:frame_len])
            expect = crc16_ccitt_false(self.buf[2:9 + length])
            self.buf = self.buf[frame_len:]
            if crc != expect:
                print(f"[decode] CRC error type=0x{ftype:02x}")
                continue
            frames.append((ftype, flags, seq, payload))
        return frames


def main():
    port, command = sys.argv[1], sys.argv[2]
    arg = sys.argv[3] if len(sys.argv) > 3 else None
    ser = serial.Serial(port, 921600, timeout=0.1)
    dec = Decoder()

    def rx():
        return dec.feed(ser.read(4096))

    if command == "hello":
        payload = json.dumps({"schema": "mibot.uart.v1", "fw_version": "peer-sim-0.1",
                              "proto_version": 1, "device_id": "sf32-sim"}).encode()
        ser.write(encode(TYPE_HELLO, FLAG_ACK_REQUEST, 1, payload))
        deadline = time.time() + 3
        while time.time() < deadline:
            for ftype, flags, seq, payload in rx():
                if ftype == TYPE_HELLO_ACK:
                    print(f"HELLO_ACK seq={seq} flags=0x{flags:02x}: {payload.decode()}")
                    return
        sys.exit("no HELLO_ACK within 3s")

    elif command == "ping":
        count = int(arg or 10)
        rtts = []
        for i in range(count):
            t0 = time.time()
            ser.write(encode(TYPE_PING, FLAG_ACK_REQUEST, i, b""))
            got = False
            while time.time() - t0 < 2 and not got:
                for ftype, flags, seq, _ in rx():
                    if ftype == TYPE_PONG and seq == i:
                        rtts.append((time.time() - t0) * 1000)
                        got = True
            if not got:
                print(f"ping {i}: TIMEOUT")
        if rtts:
            print(f"rtt min/avg/max = {min(rtts):.1f}/{sum(rtts)/len(rtts):.1f}/{max(rtts):.1f} ms ({len(rtts)}/{count})")

    elif command == "telemetry":
        seconds = int(arg or 20)
        deadline = time.time() + seconds
        while time.time() < deadline:
            for ftype, flags, seq, payload in rx():
                if ftype == TYPE_TELEMETRY:
                    print(f"[{time.strftime('%H:%M:%S')}] {payload.decode()}")
                elif ftype == TYPE_HELLO_ACK:
                    print("HELLO_ACK received — link ready")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: 语法自检**

Run: `python -m py_compile scripts/mibot_uart_peer.py`
Expected: 无输出（编译通过）。

- [ ] **Step 3: Commit**

```bash
git add scripts/mibot_uart_peer.py
git commit -m "mibot: PC-side SF32 simulator peer for UART bring-up"
```

---

### Task 10: 设备端端到端验证（spec §8.2 步骤 1）

**Files:** 无新文件（验证任务）

- [ ] **Step 1: 烧录并接线**

Run: `idf.py -p <PORT> flash monitor`
接线：USB-UART 适配器 **交叉** 连接（适配器 RX ← GPIO17，适配器 TX → GPIO18，GND 共地）。

- [ ] **Step 2: HELLO/HELLO_ACK 验证**

Run: `python scripts/mibot_uart_peer.py <COMx> hello`
Expected: 打印 `HELLO_ACK seq=1 flags=0x02: {"schema":"mibot.uart.v1","proto_version":1,"max_frame":4096,"fw_version":"0.1.0","capabilities":[...]}`
设备日志（`ESP_LOGI`）出现 `HELLO from SF32, fw=peer-sim-0.1`。

- [ ] **Step 3: PING/PONG RTT 验证**

Run: `python scripts/mibot_uart_peer.py <COMx> ping 50`
Expected: `rtt min/avg/max = x/y/z ms (50/50)`，无 TIMEOUT，无 decode CRC error。

- [ ] **Step 4: TELEMETRY 验证**

Run: `python scripts/mibot_uart_peer.py <COMx> telemetry 10`
Expected: 约 10 行 JSON，每行含 `"schema":"mibot.telemetry.v1"`、`"motion_state":"MOTION_INIT"`、`"wifi":{...}`；1 秒一行节奏。
说明：`wifi.connected` 为 `false` 是**预期内的**——M1 板卡无按键无屏幕，NVS 无凭据时无法配网（见头部"前向约束"第 4 条）；本验证不依赖 Wi-Fi，仅要求 `esp_wifi_sta_get_ap_info` 失败路径不崩溃。若需 `true`，先在 idf.py monitor/配网流程写入凭据。

- [ ] **Step 5: CRC/乱流鲁棒性抽查**

向链路注入 200 字节随机垃圾后立刻发 HELLO（对端 `hello` 命令前用任意串口工具先写垃圾；或临时在 peer 脚本 `main()` 入口加 `ser.write(b"\x00" * 200 + b"\xAA\x55\xDE\xAD" * 10)`）。
Expected: 设备不崩溃；`mibot show` 经 UART0 console（idf.py monitor 内输入）仍正常；随后 HELLO 成功。

- [ ] **Step 6: M1 DoD 确认与收尾提交**

对照 spec §8.2 步骤 1：HELLO/CRC/PING/TELEMETRY 全部通过即 M1 DoD 达成（"保活丢失 1500ms 可复现"为 M2 项，不在本计划）。
若验证中发现缺陷：修复 → 重跑本 Task 全部步骤 → 单独 commit（`fix(mibot): ...`）。

```bash
git add -A && git commit -m "mibot: M1 verified end-to-end against PC peer (hello/ping/telemetry)"
```

---

## 宿主测试快速参考

```bash
# 全部宿主测试（Tasks 1-4）：规范入口（本机无 cmake，用 zig；CMakeLists.txt 仅作可移植备份）
"D:/Storeroom/GroceryStore/Project_python/.tools/zig-x86_64-windows-0.16.0/zig.exe" c++ -std=c++17 -Imain/boards/mibot -Itest/host test/host/test_main.cpp test/host/test_frame_codec.cpp main/boards/mibot/mibot_frame_codec.cc -o test/host/build/test_mibot.exe && ./test/host/build/test_mibot.exe
# 预期最终输出：13 tests passed
```

## 后续计划（不在本文件）

- **Plan 2（M2-M3）**：ToF 阵列 + 边缘检测 + 运动安全状态机 + `command_exec`（move/rotate/stop/set_arm_pose 限幅/幂等/TTL）+ 保活租约刹车 + 控制/音频双队列。必须同时落实本文件"前向约束"三条。
- **Plan 3（M4-M6）**：AI 网关（LLM 代理/ASR/TTS/视觉）+ audio_relay + camera_capture。
