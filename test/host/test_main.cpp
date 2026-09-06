#include "test_framework.h"

int main() {
    if (GetTests().empty()) {
        std::fprintf(stderr, "no tests registered\n");
        return 1;
    }
    // 断言失败即 std::exit(1)，顺序执行即可；失败时最后一个 [RUN] 就是肇事用例
    for (auto& t : GetTests()) {
        std::printf("[ RUN  ] %s\n", t.name.c_str());
        t.fn();
        std::printf("[  OK  ] %s\n", t.name.c_str());
    }
    std::printf("%zu tests passed\n", GetTests().size());
    return 0;
}
