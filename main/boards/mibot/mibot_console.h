#ifndef MIBOT_CONSOLE_H
#define MIBOT_CONSOLE_H

class MibotLinkService;  // 全局类，定义在 mibot_link_service.h

namespace mibot {
// 非阻塞启动 esp_console REPL（UART0，自建任务）
void StartConsoleTaskAsync();
}  // namespace mibot

// 由 mibot_board.cc 实现：返回板卡持有的链路服务（启动早期可能为 nullptr）
MibotLinkService* MibotGetLinkService();

#endif
