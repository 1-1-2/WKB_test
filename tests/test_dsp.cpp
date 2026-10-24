/*
 * test_dsp.cpp -- DSP 单元测试：
 *   - 增益：线性放大、缩小、饱和 clamp（不回绕）、NaN/Inf 安全
 *   - 电平：RMS / peak 归一化到 0..1，含直流、正弦、空缓冲
 *   - 去咔哒：淡入窗口内能量连续（拼接处无跳变），跨越块边界不折断
 *   - 静音填充
 */
#include <miclink/miclink_dsp.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "test_util.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

void test_gain()
{
    /* 缩小 */
    std::vector<int16_t> x(8, 1000);
    miclink_apply_gain(x.data(), (uint32_t)x.size(), 0.5f);
    for (int16_t v : x) {
        MICLINK_CHECK_EQ(v, 500);
    }

    /* 放大并饱和：30000 * 10 = 300000 -> clamp 到 32767（绝不回绕成负数） */
    std::vector<int16_t> y(4, 30000);
    miclink_apply_gain(y.data(), (uint32_t)y.size(), 10.0f);
    for (int16_t v : y) {
        MICLINK_CHECK_EQ(v, 32767);
    }

    /* 负向饱和 */
    std::vector<int16_t> z(4, -30000);
    miclink_apply_gain(z.data(), (uint32_t)z.size(), 10.0f);
    for (int16_t v : z) {
        MICLINK_CHECK_EQ(v, -32768);
    }

    /* 单位增益不改变数据 */
    std::vector<int16_t> w = {0, 1, -1, 32767, -32768, 12345};
    std::vector<int16_t> w0 = w;
    miclink_apply_gain(w.data(), (uint32_t)w.size(), 1.0f);
    for (size_t i = 0; i < w.size(); ++i) {
        MICLINK_CHECK_EQ(w[i], w0[i]);
    }

    /* 增益 0 -> 全零 */
    miclink_apply_gain(w.data(), (uint32_t)w.size(), 0.0f);
    for (int16_t v : w) {
        MICLINK_CHECK_EQ(v, 0);
    }

    /* NaN / Inf 必须被拒绝，不把未定义值写回 */
    std::vector<int16_t> n = {1, 2, 3, 4};
    std::vector<int16_t> n0 = n;
    miclink_apply_gain(n.data(), (uint32_t)n.size(), std::numeric_limits<float>::quiet_NaN());
    miclink_apply_gain(n.data(), (uint32_t)n.size(), std::numeric_limits<float>::infinity());
    for (size_t i = 0; i < n.size(); ++i) {
        MICLINK_CHECK_EQ(n[i], n0[i]);
    }

    /* 空缓冲与 NULL 不得崩溃 */
    miclink_apply_gain(nullptr, 0u, 2.0f);
    std::vector<int16_t> e;
    miclink_apply_gain(e.data(), 0u, 2.0f);
}

void test_level()
{
    /* 空缓冲 */
    float r = -1.0f;
    float p = -1.0f;
    miclink_measure_level(nullptr, 0u, &r, &p);
    MICLINK_CHECK_EQ(r, 0.0f);
    MICLINK_CHECK_EQ(p, 0.0f);

    /* 直流 16384 -> rms = peak = 16384/32767 */
    std::vector<int16_t> dc(1000, 16384);
    miclink_measure_level(dc.data(), (uint32_t)dc.size(), &r, &p);
    MICLINK_CHECK_NEAR(r, 16384.0 / 32767.0, 1e-4);
    MICLINK_CHECK_NEAR(p, 16384.0 / 32767.0, 1e-4);

    /* 满量程方波 -> 1.0 */
    std::vector<int16_t> full(256, 32767);
    miclink_measure_level(full.data(), (uint32_t)full.size(), &r, &p);
    MICLINK_CHECK_NEAR(p, 1.0, 1e-4);

    /* 正弦：rms = A/sqrt(2)，peak = A */
    const uint32_t n = 48000u;
    std::vector<int16_t> sine(n);
    const double amp = 0.5 * 32767.0;
    for (uint32_t i = 0; i < n; ++i) {
        double v = amp * std::sin(2.0 * kPi * 1000.0 * (double)i / 48000.0);
        sine[i] = (int16_t)std::llround(v);
    }
    miclink_measure_level(sine.data(), n, &r, &p);
    MICLINK_CHECK_NEAR(p, 0.5, 5e-3);
    MICLINK_CHECK_NEAR(r, 0.5 / std::sqrt(2.0), 5e-3);

    /* 静音 -> 0 */
    std::vector<int16_t> silence(128, 0);
    miclink_measure_level(silence.data(), (uint32_t)silence.size(), &r, &p);
    MICLINK_CHECK_EQ(r, 0.0f);
    MICLINK_CHECK_EQ(p, 0.0f);

    /* 输出指针可为 NULL */
    miclink_measure_level(silence.data(), (uint32_t)silence.size(), nullptr, nullptr);
}

void test_fill_silence()
{
    std::vector<int16_t> buf(64, 12345);
    miclink_fill_silence(buf.data(), (uint32_t)buf.size());
    for (int16_t v : buf) {
        MICLINK_CHECK_EQ(v, 0);
    }
    miclink_fill_silence(nullptr, 0u);
}

/*
 * 去咔哒：能量连续性
 * 场景：第一段全 0（prev = 0）-> 通知丢包 -> 第二段突然跳到 +20000。
 * 若不处理，拼接处会有 20000 的阶跃（爆音）；处理后最大相邻跳变应 <= 20000/64 + 1。
 */
void test_declck_energy_continuity()
{
    miclink_declck d;
    miclink_declck_init(&d, 64u);
    MICLINK_CHECK_EQ(d.fade_len, 64u);

    /* 第一段：全零 128 样本。init 后自带一次淡入（从 0 淡入，值仍为 0） */
    std::vector<int16_t> a(128, 0);
    miclink_apply_declck(&d, a.data(), (uint32_t)a.size());
    for (int16_t v : a) {
        MICLINK_CHECK_EQ(v, 0);
    }

    /* 断流 */
    miclink_declck_notify_loss(&d);

    /* 第二段：恒定 +20000，长 256 样本 */
    std::vector<int16_t> b(256, 20000);
    miclink_apply_declck(&d, b.data(), (uint32_t)b.size());

    const double raw_step = 20000.0;               /* 未处理时的阶跃 */
    const double max_step = raw_step / 64.0 + 2.0; /* 淡入后允许的最大相邻跳变 */

    double worst = 0.0;
    for (size_t i = 1; i < b.size(); ++i) {
        double step = std::fabs((double)b[i] - (double)b[i - 1]);
        if (step > worst) {
            worst = step;
        }
    }
    /* 相邻跳变（含跨块边界）必须远小于原始阶跃 */
    MICLINK_CHECK(worst <= max_step);
    MICLINK_CHECK(worst <= raw_step / 16.0);

    /* 起点：第一个样本与 prev(=0) 的落差只有原始阶跃的 1/64 */
    MICLINK_CHECK(std::fabs((double)b[0]) <= max_step);
    /* 淡入窗口结束处权值恰好为 1，完全接回原始样本 */
    MICLINK_CHECK_EQ(b[63], 20000);
    MICLINK_CHECK_EQ(b[64], 20000);
    /* 淡入段单调不减（能量连续上升，无反复） */
    for (size_t i = 1; i < 64u; ++i) {
        MICLINK_CHECK(b[i] >= b[i - 1]);
    }
    /* 淡入结束后不再改动数据 */
    for (size_t i = 64u; i < b.size(); ++i) {
        MICLINK_CHECK_EQ(b[i], 20000);
    }
    MICLINK_CHECK_EQ(d.fade_remaining, 0);
}

/* 淡入窗口横跨两个数据块时，曲线不能在块边界折断 */
void test_declck_span_blocks()
{
    miclink_declck d;
    miclink_declck_init(&d, 64u);
    std::vector<int16_t> a(32, 0);
    miclink_apply_declck(&d, a.data(), (uint32_t)a.size()); /* 建立 prev = 0 */
    miclink_declck_notify_loss(&d);

    /* 每块 32 样本，淡入窗口 64 -> 横跨 2 块 */
    std::vector<int16_t> b1(32, 20000);
    std::vector<int16_t> b2(32, 20000);
    miclink_apply_declck(&d, b1.data(), (uint32_t)b1.size());
    miclink_apply_declck(&d, b2.data(), (uint32_t)b2.size());

    /* 理论值：out[j] = 20000 * (j + 1) / 64 */
    for (uint32_t j = 0; j < 32u; ++j) {
        int32_t expect = (int32_t)(20000.0 * (double)(j + 1u) / 64.0);
        MICLINK_CHECK(std::abs((int32_t)b1[j] - expect) <= 1);
    }
    for (uint32_t j = 0; j < 32u; ++j) {
        int32_t expect = (int32_t)(20000.0 * (double)(32u + j + 1u) / 64.0);
        MICLINK_CHECK(std::abs((int32_t)b2[j] - expect) <= 1);
    }
    /* 块边界：b2[0] - b1[31] 仍然只是一个淡入步长 */
    MICLINK_CHECK(std::abs((int32_t)b2[0] - (int32_t)b1[31]) <= (int32_t)(20000.0 / 64.0) + 1);
    /* 窗口末端精确接回原值 */
    MICLINK_CHECK_EQ(b2[31], 20000);
}

void test_declck_no_fade_when_continuous()
{
    miclink_declck d;
    miclink_declck_init(&d, 64u);

    /* 第一段：恒定 5000，淡入完成后 prev = 5000 */
    std::vector<int16_t> a(256, 5000);
    miclink_apply_declck(&d, a.data(), (uint32_t)a.size());
    for (size_t i = 64u; i < a.size(); ++i) {
        MICLINK_CHECK_EQ(a[i], 5000);
    }

    /* 第二段：不通知丢包，数据不应被改动 */
    std::vector<int16_t> b(64, 5000);
    miclink_apply_declck(&d, b.data(), (uint32_t)b.size());
    for (int16_t v : b) {
        MICLINK_CHECK_EQ(v, 5000);
    }
    MICLINK_CHECK_EQ(d.fade_remaining, 0);

    /* reset 视为断流：重新武装淡入 */
    miclink_declck_reset(&d);
    MICLINK_CHECK(d.fade_remaining > 0);
    std::vector<int16_t> c(128, 30000);
    miclink_apply_declck(&d, c.data(), (uint32_t)c.size());
    MICLINK_CHECK(std::fabs((double)c[0]) <= 30000.0 / 64.0 + 2.0);
    MICLINK_CHECK_EQ(c[63], 30000);
}

void test_declck_edge_cases()
{
    miclink_declck d;
    miclink_declck_init(&d, 0u); /* 0 -> 使用默认 64 */
    MICLINK_CHECK_EQ(d.fade_len, MICLINK_DECLCK_DEFAULT_FADE);

    /* 数据块比淡入窗口还短 */
    std::vector<int16_t> a(8, 0);
    miclink_apply_declck(&d, a.data(), (uint32_t)a.size());
    miclink_declck_notify_loss(&d);
    std::vector<int16_t> b(8, 16000);
    miclink_apply_declck(&d, b.data(), (uint32_t)b.size());
    MICLINK_CHECK(std::fabs((double)b[0]) <= 16000.0 / 64.0 + 2.0);
    MICLINK_CHECK_EQ(d.fade_remaining, 56); /* 64 - 8 */

    /* NULL / 空 */
    miclink_apply_declck(nullptr, nullptr, 0u);
    miclink_apply_declck(&d, nullptr, 0u);
    std::vector<int16_t> empty;
    miclink_apply_declck(&d, empty.data(), 0u);
    miclink_declck_init(nullptr, 64u);
    miclink_declck_reset(nullptr);
    miclink_declck_notify_loss(nullptr);
}

}  // namespace

MICLINK_TEST_MAIN(test_dsp, ({
    test_gain();
    test_level();
    test_fill_silence();
    test_declck_energy_continuity();
    test_declck_span_blocks();
    test_declck_no_fade_when_continuous();
    test_declck_edge_cases();
}))
