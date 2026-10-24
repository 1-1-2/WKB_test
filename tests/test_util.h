/*
 * test_util.h -- 单元测试用的极简断言 / 日志 / 确定性随机数工具。
 *
 * 不依赖 GTest（避免联网下载），全部头文件内联实现，C++17。
 */
#ifndef MICLINK_TEST_UTIL_H
#define MICLINK_TEST_UTIL_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

namespace miclink_test {

inline int &failure_count()
{
    static int count = 0;
    return count;
}

inline int &check_count()
{
    static int count = 0;
    return count;
}

inline void report_failure(const char *file, int line, const std::string &what)
{
    ++failure_count();
    std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", file, line, what.c_str());
}

inline void check_bool(bool ok, const char *expr, const char *file, int line)
{
    ++check_count();
    if (!ok) {
        report_failure(file, line, std::string("CHECK(") + expr + ") 失败");
    }
}

inline void check_eq_impl(long double a, long double b, const char *ea, const char *eb,
                          const char *file, int line)
{
    ++check_count();
    if (!(a == b)) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "CHECK_EQ(%s, %s) 失败: 实际 %.10Lg, 期望 %.10Lg",
                      ea, eb, a, b);
        report_failure(file, line, msg);
    }
}

inline void check_near_impl(long double a, long double b, long double eps,
                            const char *ea, const char *eb, const char *file, int line)
{
    ++check_count();
    long double diff = a - b;
    if (diff < 0) {
        diff = -diff;
    }
    if (!(diff <= eps)) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
                      "CHECK_NEAR(%s, %s, eps=%Lg) 失败: 实际 %.10Lg, 期望 %.10Lg, 差 %.10Lg",
                      ea, eb, eps, a, b, diff);
        report_failure(file, line, msg);
    }
}

/** @brief 确定性 32 位 xorshift 随机数（保证测试可复现）。 */
class Rng {
public:
    explicit Rng(uint32_t seed) : state_(seed != 0u ? seed : 1u) {}

    uint32_t next_u32()
    {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }

    /** @brief [0, n) 区间随机数。 */
    uint32_t next(uint32_t n)
    {
        if (n == 0u) {
            return 0u;
        }
        return next_u32() % n;
    }

    /** @brief [0, 100) 百分比判定辅助。 */
    bool chance(uint32_t percent)
    {
        return next(100u) < percent;
    }

private:
    uint32_t state_;
};

inline int finish(const char *suite)
{
    if (failure_count() == 0) {
        std::printf("[  PASSED  ] %s: %d 项断言全部通过\n", suite, check_count());
        return 0;
    }
    std::printf("[  FAILED  ] %s: %d/%d 项断言失败\n", suite, failure_count(), check_count());
    return 1;
}

}  // namespace miclink_test

#define MICLINK_CHECK(cond) \
    ::miclink_test::check_bool((cond) ? true : false, #cond, __FILE__, __LINE__)

#define MICLINK_CHECK_EQ(a, b) \
    ::miclink_test::check_eq_impl((long double)(a), (long double)(b), #a, #b, __FILE__, __LINE__)

#define MICLINK_CHECK_NEAR(a, b, eps) \
    ::miclink_test::check_near_impl((long double)(a), (long double)(b), (long double)(eps), \
                                  #a, #b, __FILE__, __LINE__)

#define MICLINK_TEST_MAIN(suite, body) \
    int main()                       \
    {                                \
        body;                        \
        return ::miclink_test::finish(#suite); \
    }

#endif /* MICLINK_TEST_UTIL_H */
