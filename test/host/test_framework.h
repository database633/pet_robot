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
// 注意：宏定义末尾的 ';' 是必需的（计划文本遗漏，按计划原文无法编译）；用法处带不带 ';' 均可
#define MIBOT_TEST(name) \
    static const int reg_##name = RegisterTest(#name, name);
#define EXPECT_EQ(a, b) do { \
    auto&& mibot_va_ = (a); auto&& mibot_vb_ = (b); \
    if (!(mibot_va_ == mibot_vb_)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s != %s (got %llu, want %llu)\n", \
                     __FILE__, __LINE__, #a, #b, \
                     static_cast<unsigned long long>(mibot_va_), \
                     static_cast<unsigned long long>(mibot_vb_)); \
        std::exit(1); \
    } } while (0)
#define EXPECT_TRUE(x) do { \
    if (!(x)) { \
        std::fprintf(stderr, "FAIL %s:%d: EXPECT_TRUE(%s)\n", __FILE__, __LINE__, #x); \
        std::exit(1); \
    } } while (0)
