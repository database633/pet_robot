#ifndef MIBOT_FRAME_CODEC_H
#define MIBOT_FRAME_CODEC_H
// Mibot UART 帧编解码。纯 C++，禁止包含任何 ESP-IDF 头（宿主单测直接编译本文件）。
// 帧格式：《Mibot 桌面机器人状态机与控制接口规范》§3.2
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mibot {
// CRC-16/CCITT-FALSE（覆盖 VERSION..PAYLOAD）
uint16_t Crc16(const uint8_t* data, size_t len);
// 增量式：先算前段，再续算后段（帧头与 payload 分离存储时用）
uint16_t Crc16Update(uint16_t crc, const uint8_t* data, size_t len);

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
// 契约：
//  - 每次 Feed 批次后必须循环 PopFrame 直到返回 false，否则 frames_ 无界增长
//  - Reset() 清理解析状态与帧队列，但保留 error_count_（生命周期级诊断计数，故意的）
//  - 非线程安全：Feed/PopFrame 仅限单一任务调用（设备侧为 RX 任务）
//  - EncodeFrame 前置条件：payload.size() <= 0xFFFF（LEN 字段 16 位；4096 上限由解码端强制）
//  - 已知限制（spec 固有）：完整假 SOF（AA 55 + 恰能通过版本/长度校验的假头）会吞掉
//    后续真实帧字节且不计数；丢弃后从下一个 SOF 重新同步。噪声流下 C++ 端与 Python
//    对端可能丢不同的帧；M2 考虑对等重扫（丢弃假头时回退重扫）。
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
}  // namespace mibot
#endif  // MIBOT_FRAME_CODEC_H
