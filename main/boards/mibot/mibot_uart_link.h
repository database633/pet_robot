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
