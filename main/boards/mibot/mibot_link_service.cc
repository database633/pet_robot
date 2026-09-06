#include "mibot_link_service.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cJSON.h>

#include "mibot_config.h"

#define TAG "MibotLink"

using mibot::Frame;
using mibot::FrameFlags;
using mibot::FrameType;

void MibotLinkService::Start(mibot::MibotUartLink& link) {
    link_ = &link;
    link_->Start(MIBOT_UART_NUM, MIBOT_UART_TX_GPIO, MIBOT_UART_RX_GPIO, MIBOT_UART_BAUD,
                 [this](const Frame& f) { OnFrame(f); });

    // 专用遥测任务：独立栈，避免 esp_timer 共享任务的栈限制
    xTaskCreate(TelemetryTaskEntry, "mibot_tel", 4096, this, 4, nullptr);
}

void MibotLinkService::SendJson(FrameType type, uint8_t flags, uint16_t seq_reply, const std::string& json) {
    Frame f;
    f.type = type;
    f.flags = flags;
    f.seq = seq_reply;
    f.payload.assign(json.begin(), json.end());
    link_->Send(f);
}

void MibotLinkService::OnFrame(const Frame& frame) {
    switch (frame.type) {
        case mibot::kFrameHello: {
            // SF32 → ESP32：{"schema":"mibot.uart.v1","fw_version":"...","proto_version":1,"device_id":"..."}
            cJSON* hello = cJSON_ParseWithLength(reinterpret_cast<const char*>(frame.payload.data()),
                                                 frame.payload.size());
            const char* peer_fw = "?";
            if (hello) {
                cJSON* fw = cJSON_GetObjectItem(hello, "fw_version");
                if (cJSON_IsString(fw)) peer_fw = fw->valuestring;
            }
            ESP_LOGI(TAG, "HELLO from SF32, fw=%s seq=%u", peer_fw, frame.seq);
            if (hello) cJSON_Delete(hello);

            cJSON* ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "schema", "mibot.uart.v1");
            cJSON_AddNumberToObject(ack, "proto_version", 1);
            cJSON_AddNumberToObject(ack, "max_frame", 4096);
            cJSON_AddStringToObject(ack, "fw_version", MIBOT_FW_VERSION);
            cJSON* caps = cJSON_AddArrayToObject(ack, "capabilities");
            cJSON_AddItemToArray(caps, cJSON_CreateString("audio_relay"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("motion"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("ai_gateway"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("camera"));
            char* json = cJSON_PrintUnformatted(ack);
            std::string payload(json);
            cJSON_free(json);
            cJSON_Delete(ack);
            SendJson(mibot::kFrameHelloAck, FrameFlags::kAck, frame.seq, payload);
            ready_ = true;
            break;
        }
        case mibot::kFramePing:
            // PONG：seq 回写 = PING 的 seq，便于对端测 RTT
            SendJson(mibot::kFramePong, FrameFlags::kAck, frame.seq, "{}");
            break;
        default:
            // M1 尚无 COMMAND/AI 处理：按《规范》§5.3 回 E_UNSUPPORTED
            SendJson(mibot::kFrameNack, FrameFlags::kError, frame.seq,
                     std::string("{\"schema\":\"mibot.uart.v1\",\"error\":{\"code\":\"E_UNSUPPORTED\","
                                 "\"message\":\"type 0x") + std::to_string(frame.type) + " not supported in fw " +
                     MIBOT_FW_VERSION + "\"}}");
            break;
    }
}

void MibotLinkService::SendTelemetry() {
    double ts_ms = static_cast<double>(esp_timer_get_time()) / 1000.0;  // int64，避免 int32 回绕
    int rssi = 0;
    bool wifi_connected = false;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        wifi_connected = true;
        rssi = ap.rssi;
    }
    cJSON* tel = cJSON_CreateObject();
    cJSON_AddStringToObject(tel, "schema", "mibot.telemetry.v1");
    cJSON_AddNumberToObject(tel, "ts_ms", ts_ms);
    cJSON_AddStringToObject(tel, "behavior_state", "-");          // SF32 职责，ESP32 未知
    cJSON_AddStringToObject(tel, "motion_state", "MOTION_INIT");  // M2 接入运动状态机后更新
    cJSON_AddNumberToObject(tel, "battery_mv", 0);                // M3 接入 ADC 后更新
    cJSON_AddNullToObject(tel, "tof");
    cJSON* motor = cJSON_AddObjectToObject(tel, "motor");
    cJSON_AddStringToObject(motor, "left", "off");
    cJSON_AddStringToObject(motor, "right", "off");
    cJSON_AddNullToObject(tel, "servo");
    cJSON* wifi = cJSON_AddObjectToObject(tel, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_connected);
    cJSON_AddNumberToObject(wifi, "rssi_dbm", rssi);
    char* json = cJSON_PrintUnformatted(tel);
    std::string payload(json);
    cJSON_free(json);
    cJSON_Delete(tel);
    SendJson(mibot::kFrameTelemetry, 0, tx_seq_++, payload);
}

void MibotLinkService::TelemetryTaskEntry(void* arg) {
    static_cast<MibotLinkService*>(arg)->TelemetryLoop();
}

void MibotLinkService::TelemetryLoop() {
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        SendTelemetry();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));  // 1Hz，不漂移
    }
}
