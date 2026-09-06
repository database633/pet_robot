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
                    state_ = (byte == kSof0) ? kSync1 : kSync0;  // 真 SOF 前多余的 0xAA 可容忍；完整假 SOF 会吞 7 字节后在下一个 SOF 重同步
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

}  // namespace mibot
