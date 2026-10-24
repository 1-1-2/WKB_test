/*
 * test_jitter_buffer.cpp -- 自适应抖动缓冲测试。
 *
 * 场景矩阵（36 组 >= 要求的 12 组）：
 *   主矩阵 32 组：48 kHz / 10 ms 帧
 *     抖动 {0, 20, 50, 100} ms × 丢包 {0, 1, 5, 10} % × {正常, 乱序+重复}
 *   附加 4 组：16 kHz / 20 ms 帧 × 抖动 {0, 100} ms × 丢包 {0, 10} %
 *
 * 每组断言：
 *   1) 稳态目标延迟落在 50~100 ms（min=50, max=100）
 *   2) 抖动消失（后接 1500 帧零抖动零丢包）后目标延迟回落到 <= 120 ms（硬要求）
 *      并且实际回落到 min 附近
 *   3) 输出连续：真实帧的 seq 严格递增且必须是真实发送过的帧（无重复/无错乱）
 *   4) 无长卡顿：产出帧数接近理论值，重新缓冲(stall)次数很少
 *   5) 丢包率与注入的丢包率一致（允许少量启动瞬态与迟到丢弃）
 */
#include <miclink/miclink_jitter.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "test_util.h"

namespace {

struct SimResult {
    uint32_t jitter_ms = 0u;
    uint32_t loss_pct = 0u;
    uint32_t reorder = 0u;
    uint32_t frame_ms = 0u;
    uint32_t sample_rate = 0u;
    double steady_target_ms = 0.0;
    double steady_depth_ms = 0.0;
    uint32_t final_target_ms = 0u;
    uint32_t emitted = 0u;
    uint32_t missing = 0u;
    uint32_t real = 0u;
    uint32_t stalls = 0u;
    uint32_t continuity_errors = 0u;
    uint32_t duplicates = 0u;
    uint32_t late = 0u;
};

struct Event {
    uint32_t t;
    uint32_t seq;
};

SimResult run_sim(uint32_t jitter_ms, uint32_t loss_pct, uint32_t reorder_mode,
                  uint32_t frame_ms, uint32_t sample_rate, uint32_t seed)
{
    const uint32_t kMain = 3000u;   /* 主阶段帧数（有抖动/丢包） */
    const uint32_t kQuiet = 1500u;  /* 抖动消失后的观察阶段帧数 */
    const uint32_t kTotal = kMain + kQuiet;
    const uint32_t spf = sample_rate * frame_ms / 1000u; /* channels = 1 */

    miclink_jb_config cfg;
    cfg.target_delay_ms = 50u;
    cfg.min_delay_ms = 50u;
    cfg.max_delay_ms = 100u;
    cfg.frame_ms = frame_ms;
    cfg.capacity_frames = 40u;
    cfg.sample_rate = sample_rate;
    cfg.channels = 1u;

    const size_t need = miclink_jb_storage_bytes(&cfg);
    MICLINK_CHECK(need > 0u);
    std::vector<uint8_t> storage(need, 0u);
    miclink_jitter jb;
    const int rc = miclink_jb_init(&jb, &cfg, storage.data(), need);
    MICLINK_CHECK_EQ(rc, 0);
    MICLINK_CHECK_EQ(jb.samples_per_frame, spf);

    /* ---- 生成到达计划 ---- */
    std::vector<uint8_t> sent(kTotal, 0u);
    std::vector<uint32_t> arrival(kTotal, 0u);
    miclink_test::Rng rng(seed);

    for (uint32_t i = 0u; i < kTotal; ++i) {
        const bool quiet = (i >= kMain);
        const uint32_t j = quiet ? 0u : jitter_ms;
        if (!quiet && rng.chance(loss_pct)) {
            continue; /* 丢包：这一帧永远不到达 */
        }
        sent[i] = 1u;
        int32_t offset = 0;
        if (j > 0u) {
            /* 单向抖动：到达延迟在 [20 - j/2, 20 + j/2] 内均匀分布，
               相邻帧延迟差最大为 j，可以产生真正的乱序。 */
            offset = static_cast<int32_t>(rng.next(j + 1u)) - static_cast<int32_t>(j / 2u);
        }
        int64_t a = static_cast<int64_t>(i) * static_cast<int64_t>(frame_ms) + 20 + offset;
        if (a < 0) {
            a = 0;
        }
        arrival[i] = static_cast<uint32_t>(a);
    }

    if (reorder_mode != 0u) {
        /* 乱序：15% 概率交换相邻两帧的到达时刻 */
        for (uint32_t i = 1u; i < kMain; ++i) {
            if (sent[i] && sent[i - 1u] && rng.chance(15u)) {
                std::swap(arrival[i], arrival[i - 1u]);
            }
        }
    }

    std::vector<Event> events;
    events.reserve(kTotal * 2u);
    for (uint32_t i = 0u; i < kTotal; ++i) {
        if (!sent[i]) {
            continue;
        }
        events.push_back(Event{arrival[i], i});
        if ((reorder_mode != 0u) && (i < kMain) && rng.chance(10u)) {
            events.push_back(Event{arrival[i] + 1u, i}); /* 重复包 */
        }
    }
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        return a.t < b.t;
    });

    /* ---- 时间轴推进 ---- */
    SimResult res;
    res.jitter_ms = jitter_ms;
    res.loss_pct = loss_pct;
    res.reorder = reorder_mode;
    res.frame_ms = frame_ms;
    res.sample_rate = sample_rate;

    std::vector<int16_t> src(spf, 0);
    std::vector<int16_t> out(spf, 0);
    size_t ei = 0u;
    const uint32_t main_end_ms = kMain * frame_ms;
    const uint32_t end_ms = kTotal * frame_ms + 1000u;
    double target_sum = 0.0;
    double depth_sum = 0.0;
    uint32_t sample_n = 0u;
    uint32_t last_real_seq = 0u;
    bool have_last_real = false;

    for (uint32_t now = 0u; now <= end_ms; ++now) {
        /* 投递本毫秒到达的所有包 */
        while (ei < events.size() && events[ei].t <= now) {
            const uint32_t seq = events[ei].seq;
            const int16_t v = static_cast<int16_t>(seq & 0x7FFFu);
            for (uint32_t k = 0u; k < spf; ++k) {
                src[k] = v; /* 用 seq 填充整帧，便于校验输出连续性 */
            }
            const int prc = miclink_jb_put(&jb, seq, src.data(), spf, now);
            MICLINK_CHECK(prc == 0 || prc == 1 || prc == 2);
            ++ei;
        }

        /* 播放时钟：只要到点就取一帧 */
        for (;;) {
            int missing_flag = 0;
            const int grc = miclink_jb_get(&jb, out.data(), spf, &missing_flag, now);
            MICLINK_CHECK(grc == 0 || grc == 1);
            if (grc != 1) {
                break;
            }
            ++res.emitted;
            if (missing_flag) {
                ++res.missing;
            } else {
                ++res.real;
                const uint32_t seq = static_cast<uint32_t>(static_cast<uint16_t>(out[0]));
                if ((seq >= kTotal) || !sent[seq]) {
                    ++res.continuity_errors; /* 拿到了没发送过的帧 -> 错乱 */
                } else if (have_last_real && (seq <= last_real_seq)) {
                    ++res.continuity_errors; /* 顺序错乱或重复消费 */
                }
                last_real_seq = seq;
                have_last_real = true;
            }
        }

        /* 稳态统计取主阶段后半段 */
        if ((now >= main_end_ms / 2u) && (now < main_end_ms)) {
            target_sum += static_cast<double>(miclink_jb_target_delay_ms(&jb));
            depth_sum += static_cast<double>(miclink_jb_depth_ms(&jb));
            ++sample_n;
        }
    }

    MICLINK_CHECK(sample_n > 0u);
    res.steady_target_ms = target_sum / static_cast<double>(sample_n);
    res.steady_depth_ms = depth_sum / static_cast<double>(sample_n);
    res.final_target_ms = miclink_jb_target_delay_ms(&jb);
    res.stalls = jb.stall_count;
    res.duplicates = static_cast<uint32_t>(jb.frames_duplicate);
    res.late = static_cast<uint32_t>(jb.frames_late);
    return res;
}

std::vector<SimResult> g_results;

void run_matrix()
{
    const uint32_t jitters[4] = {0u, 20u, 50u, 100u};
    const uint32_t losses[4] = {0u, 1u, 5u, 10u};

    std::printf("  %-8s %-6s %-10s %-14s %-14s %-10s %-8s %-8s %-8s\n",
                "抖动ms", "丢包%", "模式", "稳态目标ms", "稳态深度ms",
                "回落后ms", "占位帧", "真实帧", "重缓冲");
    for (uint32_t ji = 0u; ji < 4u; ++ji) {
        for (uint32_t li = 0u; li < 4u; ++li) {
            for (uint32_t mode = 0u; mode < 2u; ++mode) {
                const uint32_t seed = 0x9E3779B9u + (ji * 977u) + (li * 131u) + (mode * 17u);
                SimResult r = run_sim(jitters[ji], losses[li], mode, 10u, 48000u, seed);
                g_results.push_back(r);
                std::printf("  %-8u %-6u %-10s %-14.1f %-14.1f %-10u %-8u %-8u %-8u\n",
                            r.jitter_ms, r.loss_pct, (mode ? "乱序+重复" : "正常"),
                            r.steady_target_ms, r.steady_depth_ms, r.final_target_ms,
                            r.missing, r.real, r.stalls);
            }
        }
    }

    /* 附加：16 kHz / 20 ms 帧 */
    const uint32_t extra_jitter[2] = {0u, 100u};
    const uint32_t extra_loss[2] = {0u, 10u};
    for (uint32_t a = 0u; a < 2u; ++a) {
        for (uint32_t b = 0u; b < 2u; ++b) {
            const uint32_t seed = 0x1234567u + a * 31u + b * 7u;
            SimResult r = run_sim(extra_jitter[a], extra_loss[b], 0u, 20u, 16000u, seed);
            g_results.push_back(r);
            std::printf("  %-8u %-6u %-10s %-14.1f %-14.1f %-10u %-8u %-8u %-8u\n",
                        r.jitter_ms, r.loss_pct, "16k/20ms",
                        r.steady_target_ms, r.steady_depth_ms, r.final_target_ms,
                        r.missing, r.real, r.stalls);
        }
    }
}

void assert_all()
{
    MICLINK_CHECK_EQ(g_results.size(), 36u); /* >= 12 组 */

    for (const SimResult &r : g_results) {
        /* 1) 稳态目标延迟必须在 [50, 100] ms */
        MICLINK_CHECK(r.steady_target_ms >= 49.5);
        MICLINK_CHECK(r.steady_target_ms <= 100.5);

        /* 2) 抖动消失后必须回落：硬要求 <= 120 ms；
              实际上应当回落到 min(50) 附近（允许 2 帧的残差） */
        MICLINK_CHECK(r.final_target_ms <= 120u);
        MICLINK_CHECK(r.final_target_ms <= 50u + 2u * r.frame_ms);

        /* 3) 输出连续性：不允许出现未发送 / 乱序 / 重复的帧 */
        MICLINK_CHECK_EQ(r.continuity_errors, 0u);

        /* 4) 无长卡顿：产出帧数应接近理论值（4500 帧减去起播缓冲） */
        const uint32_t theoretical = 3000u + 1500u;
        MICLINK_CHECK(r.emitted + 30u >= theoretical - (100u / r.frame_ms));
        MICLINK_CHECK(r.stalls <= 40u);

        /* 5) 占位帧数量应与注入丢包率吻合（允许启动瞬态与迟到丢弃的少量偏差） */
        const double injected = 0.01 * static_cast<double>(r.loss_pct) * 3000.0;
        const double tolerance = injected * 0.5 + 40.0;
        MICLINK_CHECK(static_cast<double>(r.missing) <= injected + tolerance);
        if (r.loss_pct == 0u) {
            MICLINK_CHECK(r.missing <= 40u);
        }
    }
}

}  // namespace

MICLINK_TEST_MAIN(test_jitter_buffer, ({
    std::printf("  自适应抖动缓冲场景矩阵（%d 组）:\n", 36);
    run_matrix();
    assert_all();
}))
