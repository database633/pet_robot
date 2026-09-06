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
