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
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            Frame assembled;
            if (frags_.Feed(f, now, assembled)) {  // 全部帧走同一入口：非分片帧在 Feed 内直通
                handler_(assembled);
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
