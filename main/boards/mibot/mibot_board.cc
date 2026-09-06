#include <esp_log.h>

#include "wifi_board.h"
#include "codecs/dummy_audio_codec.h"

#include "mibot_config.h"
#include "mibot_uart_link.h"
#include "mibot_link_service.h"

#define TAG "MibotBoard"

// Mibot ESP32-S3：AI 网关 + 运动安全执行器（M1 仅骨架）。
// 无本地麦克风/喇叭/屏幕：Display 用基类 NoDisplay，音频用 DummyAudioCodec
// （Read/Write 恒返 0；audio_service.cc:308 的 10ms 兜底延时保证不忙等）。
class MibotBoard : public WifiBoard {
private:
    DummyAudioCodec codec_{AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE};
    mibot::MibotUartLink uart_link_;
    MibotLinkService link_service_;

public:
    MibotBoard() {
        ESP_LOGI(TAG, "Mibot ESP32-S3 board init, fw=%s", MIBOT_FW_VERSION);
        link_service_.Start(uart_link_);  // M1：链路服务随板卡启动（不依赖 Wi-Fi）
    }

    ~MibotBoard() override = default;

    std::string GetBoardType() override { return "mibot-esp32s3"; }

    AudioCodec* GetAudioCodec() override { return &codec_; }

    // GetNetworkStateIcon/SetPowerSaveLevel 用 WifiBoard 默认实现；
    // M2（运动安全）再覆写 SetPowerSaveLevel 保持 Wi-Fi 常连接。
};

DECLARE_BOARD(MibotBoard);
