#pragma once
// 极简测试框架：无外部依赖，断言失败即退出非零
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

struct TestCase { std::string name; std::function<void()> fn; };
inline std::vector<TestCase>& GetTests() { static std::vector<TestCase> tests; return tests; }
inline int RegisterTest(const char* name, std::function<void()> fn) {
    GetTests().push_back({name, std::move(fn)});
    return 0;
}
#define MIBOT_TEST(name) \
    static const int reg_##name = RegisterTest(#name, name);
#define EXPECT_EQ(a, b) do { \
    if (!((a) == (b))) { \
        std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        std::exit(1); \
    } } while (0)
#define EXPECT_TRUE(x) EXPECT_EQ(static_cast<bool>(x), true)
