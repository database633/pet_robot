#include "mibot_frame_codec.h"
namespace mibot {

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

}  // namespace mibot
