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
}  // namespace mibot
#endif  // MIBOT_FRAME_CODEC_H
