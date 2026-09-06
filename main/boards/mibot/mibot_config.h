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
