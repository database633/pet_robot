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
    // 解码错误透出（mibot show 诊断用）
    uint32_t ErrorCount() const { return link_ ? link_->DecoderErrorCount() : 0; }

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
