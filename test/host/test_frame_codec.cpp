#include "test_framework.h"
#include "mibot_frame_codec.h"

using namespace mibot;

static void test_scaffold() { EXPECT_EQ(1, 1); }
MIBOT_TEST(test_scaffold)

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

// 帧长 = SOF(2) + VERSION(1) + TYPE(1) + FLAGS(1) + SEQ(2) + LENGTH(2) + PAYLOAD(n) + CRC(2) = 11 + n
static Frame MakeTestFrame(uint16_t seq, size_t payload_len) {
    Frame f;
    f.type = kFrameCommand;
    f.flags = FrameFlags::kAckRequest;
    f.seq = seq;
    for (size_t i = 0; i < payload_len; ++i) f.payload.push_back(static_cast<uint8_t>(i & 0xFF));
    return f;
}

static void test_crc_split_equivalence() {
    const char* s = "123456789";
    EXPECT_EQ(Crc16Update(Crc16Update(0xFFFF, reinterpret_cast<const uint8_t*>(s), 4),
                          reinterpret_cast<const uint8_t*>(s) + 4, 5), 0x29B1);
}
MIBOT_TEST(test_crc_split_equivalence)

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
