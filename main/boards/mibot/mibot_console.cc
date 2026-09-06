#include "mibot_console.h"

#include <cstring>

#include <esp_console.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/uart.h>

#include "mibot_config.h"
#include "mibot_link_service.h"

namespace mibot {

static int CmdMibot(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "show") != 0) {
        printf("usage: mibot show\n");
        return 1;
    }
    MibotLinkService* svc = MibotGetLinkService();
    printf("fw=%s uart=%d baud=%d tx=%d rx=%d ready=%s errors=%u\n",
           MIBOT_FW_VERSION, MIBOT_UART_NUM, MIBOT_UART_BAUD,
           MIBOT_UART_TX_GPIO, MIBOT_UART_RX_GPIO,
           (svc && svc->IsReady()) ? "yes" : "no",
           (unsigned)(svc ? svc->ErrorCount() : 0u));
    return 0;
}

static void StartConsoleTask() {
    // REPL 引导同款做法见 main/boards/sensecap-watcher/sensecap_watcher.cc:436
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.max_cmdline_length = 512;
    repl_config.prompt = "mibot> ";
    esp_console_register_help_command();

    esp_console_cmd_t cmd = {};  // 成员逐个赋值，规避 IDF 版本间结构体字段顺序差异
    cmd.command = "mibot";
    cmd.help = "Mibot board diagnostics";
    cmd.hint = "show";
    cmd.func = &CmdMibot;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

    esp_console_repl_t* repl = nullptr;
    esp_console_dev_uart_config_t uart_cfg = {};  // UART0 = 烧录/日志口
    uart_cfg.baud_rate = 115200;
    uart_cfg.tx_gpio = GPIO_NUM_43;
    uart_cfg.rx_gpio = GPIO_NUM_44;
    uart_cfg.port = UART_NUM_0;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

static void ConsoleTaskEntry(void*) {
    StartConsoleTask();
    vTaskDelete(nullptr);
}

void StartConsoleTaskAsync() {
    // xTaskCreate 成功返回 pdPASS(1) 而非 ESP_OK(0)，转成 esp_err_t 再交给 ESP_ERROR_CHECK
    BaseType_t cli_ok = xTaskCreate(ConsoleTaskEntry, "mibot_cli", 6144, nullptr, 3, nullptr);
    ESP_ERROR_CHECK(cli_ok == pdPASS ? ESP_OK : ESP_FAIL);
}

}  // namespace mibot
