/*
 * test_ring_buffer.cpp -- SPSC 环形缓冲单元测试。
 *
 * 覆盖：
 *   1) 初始化 / 容量与 mask / 空读 / 满写 / 短写
 *   2) wrap-around（跨越缓冲区末尾回绕）
 *   3) 帧对齐（多声道帧不被撕开）
 *   4) read_fill_silence 的补零与返回值语义
 *   5) reset
 *   6) 并发 SPSC 压力：2 线程、1000 万帧，断言
 *        丢样本 = 0、输出严格按序、无撕裂
 */
#include <womic/womic_ring.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "test_util.h"

namespace {

constexpr uint32_t kCapacity = 1024u;   /* 必须是 2 的幂 */
constexpr uint32_t kFrameBytes = 4u;    /* 立体声：2 * sizeof(int16_t) */
constexpr uint32_t kSamplesPerFrame = kFrameBytes / 2u;

void test_init_and_basic()
{
    std::vector<int16_t> storage(kCapacity * kSamplesPerFrame, 0);
    womic_ring r;

    int rc = womic_ring_init(&r, storage.data(), kCapacity, kFrameBytes);
    WOMIC_CHECK_EQ(rc, WOMIC_RING_OK);
    WOMIC_CHECK_EQ(r.capacity, kCapacity);
    WOMIC_CHECK_EQ(r.mask, kCapacity - 1u);
    WOMIC_CHECK_EQ(womic_ring_frame_samples(&r), kSamplesPerFrame);
    WOMIC_CHECK_EQ(womic_ring_available(&r), 0u);
    WOMIC_CHECK_EQ(womic_ring_free(&r), kCapacity);

    /* 非 2 的幂 -> 向下取整 */
    womic_ring r2;
    rc = womic_ring_init(&r2, storage.data(), 1000u, kFrameBytes);
    WOMIC_CHECK_EQ(rc, WOMIC_RING_OK);
    WOMIC_CHECK_EQ(r2.capacity, 512u);

    /* 非法帧字节数 */
    womic_ring r3;
    WOMIC_CHECK_EQ(womic_ring_init(&r3, storage.data(), 16u, 0u), WOMIC_RING_ERR_BAD_FRAME);
    WOMIC_CHECK_EQ(womic_ring_init(&r3, storage.data(), 16u, 3u), WOMIC_RING_ERR_BAD_FRAME);
    WOMIC_CHECK_EQ(womic_ring_init(nullptr, storage.data(), 16u, 2u), WOMIC_RING_ERR_NULL);
}

void test_write_read_roundtrip()
{
    std::vector<int16_t> storage(kCapacity * kSamplesPerFrame, 0);
    womic_ring r;
    WOMIC_CHECK_EQ(womic_ring_init(&r, storage.data(), kCapacity, kFrameBytes), WOMIC_RING_OK);

    std::vector<int16_t> in(8u * kSamplesPerFrame);
    std::vector<int16_t> out(8u * kSamplesPerFrame, -1);
    for (uint32_t i = 0; i < 8u; ++i) {
        in[i * kSamplesPerFrame + 0u] = static_cast<int16_t>(1000 + i);
        in[i * kSamplesPerFrame + 1u] = static_cast<int16_t>(-(1000 + static_cast<int>(i)));
    }

    WOMIC_CHECK_EQ(womic_ring_write(&r, in.data(), 8u), 8u);
    WOMIC_CHECK_EQ(womic_ring_available(&r), 8u);
    WOMIC_CHECK_EQ(womic_ring_free(&r), kCapacity - 8u);
    WOMIC_CHECK_EQ(womic_ring_read(&r, out.data(), 8u), 8u);
    for (uint32_t i = 0; i < 8u * kSamplesPerFrame; ++i) {
        WOMIC_CHECK_EQ(out[i], in[i]);
    }
    WOMIC_CHECK_EQ(womic_ring_available(&r), 0u);
    WOMIC_CHECK_EQ(r.written_frames, 8u);
    WOMIC_CHECK_EQ(r.read_frames, 8u);
    WOMIC_CHECK_EQ(r.dropped_frames, 0u);
}

void test_empty_and_full()
{
    std::vector<int16_t> storage(8u * kSamplesPerFrame, 0);
    womic_ring r;
    WOMIC_CHECK_EQ(womic_ring_init(&r, storage.data(), 8u, kFrameBytes), WOMIC_RING_OK);

    /* 空读 */
    std::vector<int16_t> out(4u * kSamplesPerFrame, -1);
    WOMIC_CHECK_EQ(womic_ring_read(&r, out.data(), 4u), 0u);

    /* 满写：容量 8，请求 16 -> 只能写 8 */
    std::vector<int16_t> in(16u * kSamplesPerFrame, 7);
    WOMIC_CHECK_EQ(womic_ring_write(&r, in.data(), 16u), 8u);
    WOMIC_CHECK_EQ(womic_ring_free(&r), 0u);
    WOMIC_CHECK_EQ(womic_ring_write(&r, in.data(), 1u), 0u); /* 已满，写不进 */

    /* 读 3 帧后可再写 3 帧 */
    WOMIC_CHECK_EQ(womic_ring_read(&r, out.data(), 3u), 3u);
    WOMIC_CHECK_EQ(womic_ring_free(&r), 3u);
    WOMIC_CHECK_EQ(womic_ring_write(&r, in.data(), 5u), 3u);
    WOMIC_CHECK_EQ(womic_ring_available(&r), 8u);

    /* 记账：写端显式记录丢帧 */
    womic_ring_add_dropped(&r, 2u);
    WOMIC_CHECK_EQ(r.dropped_frames, 2u);
}

void test_wrap_around_and_frame_alignment()
{
    /* 小容量（4 帧）反复读写，强制每次都跨越缓冲区末尾 */
    constexpr uint32_t cap = 4u;
    std::vector<int16_t> storage(cap * kSamplesPerFrame, 0);
    womic_ring r;
    WOMIC_CHECK_EQ(womic_ring_init(&r, storage.data(), cap, kFrameBytes), WOMIC_RING_OK);

    std::vector<int16_t> in(3u * kSamplesPerFrame, 0);
    std::vector<int16_t> out(3u * kSamplesPerFrame, 0);

    for (uint32_t round = 0u; round < 1000u; ++round) {
        for (uint32_t i = 0u; i < 3u; ++i) {
            int16_t v = static_cast<int16_t>((round * 3u + i) & 0x7FFF);
            in[i * kSamplesPerFrame + 0u] = v;
            in[i * kSamplesPerFrame + 1u] = static_cast<int16_t>(-v);
        }
        WOMIC_CHECK_EQ(womic_ring_write(&r, in.data(), 3u), 3u);
        WOMIC_CHECK_EQ(womic_ring_read(&r, out.data(), 3u), 3u);
        for (uint32_t i = 0u; i < 3u * kSamplesPerFrame; ++i) {
            WOMIC_CHECK_EQ(out[i], in[i]); /* 帧对齐：高低两半必须成对出现 */
        }
    }
}

void test_read_fill_silence()
{
    std::vector<int16_t> storage(kCapacity * kSamplesPerFrame, 0);
    womic_ring r;
    WOMIC_CHECK_EQ(womic_ring_init(&r, storage.data(), kCapacity, kFrameBytes), WOMIC_RING_OK);

    std::vector<int16_t> in(3u * kSamplesPerFrame, 1234);
    WOMIC_CHECK_EQ(womic_ring_write(&r, in.data(), 3u), 3u);

    std::vector<int16_t> out(8u * kSamplesPerFrame, -1);
    uint32_t real = womic_ring_read_fill_silence(&r, out.data(), 8u);
    WOMIC_CHECK_EQ(real, 3u); /* 只有 3 帧是真实的 */
    for (uint32_t i = 0u; i < 3u * kSamplesPerFrame; ++i) {
        WOMIC_CHECK_EQ(out[i], 1234);
    }
    for (uint32_t i = 3u * kSamplesPerFrame; i < 8u * kSamplesPerFrame; ++i) {
        WOMIC_CHECK_EQ(out[i], 0); /* 不足部分补零 */
    }
    WOMIC_CHECK_EQ(womic_ring_available(&r), 0u);
}

void test_reset()
{
    std::vector<int16_t> storage(kCapacity * kSamplesPerFrame, 0);
    womic_ring r;
    WOMIC_CHECK_EQ(womic_ring_init(&r, storage.data(), kCapacity, kFrameBytes), WOMIC_RING_OK);

    std::vector<int16_t> in(5u * kSamplesPerFrame, 999);
    WOMIC_CHECK_EQ(womic_ring_write(&r, in.data(), 5u), 5u);
    womic_ring_add_dropped(&r, 4u);

    womic_ring_reset(&r);
    WOMIC_CHECK_EQ(womic_ring_available(&r), 0u);
    WOMIC_CHECK_EQ(womic_ring_free(&r), kCapacity);
    WOMIC_CHECK_EQ(r.written_frames, 0u);
    WOMIC_CHECK_EQ(r.read_frames, 0u);
    WOMIC_CHECK_EQ(r.dropped_frames, 0u);
}

/*
 * 并发 SPSC 压力测试：
 *   - 每帧 2 个 int16，合起来编码一个 32 位递增计数器；
 *   - 消费者逐帧还原计数器，必须严格等于期望序列（0,1,2,...），
 *     任何丢失 / 重复 / 乱序 / 撕裂（高低两半来自不同帧）都会被检出；
 *   - 总量 1000 万帧。
 */
void test_concurrent_stress()
{
    constexpr uint64_t kTotalFrames = 10000000ull; /* >= 1000 万帧 */
    constexpr uint32_t kChunk = 64u;

    std::vector<int16_t> storage(kCapacity * kSamplesPerFrame, 0);
    womic_ring ring;
    WOMIC_CHECK_EQ(womic_ring_init(&ring, storage.data(), kCapacity, kFrameBytes), WOMIC_RING_OK);

    std::atomic<uint32_t> producer_errors{0u};
    std::atomic<uint32_t> consumer_errors{0u};
    std::atomic<uint64_t> consumed{0ull};

    auto encode = [](uint32_t counter, int16_t *dst) {
        dst[0u] = static_cast<int16_t>(static_cast<uint16_t>(counter & 0xFFFFu));
        dst[1u] = static_cast<int16_t>(static_cast<uint16_t>((counter >> 16) & 0xFFFFu));
    };
    auto decode = [](const int16_t *src) -> uint32_t {
        uint32_t lo = static_cast<uint16_t>(src[0u]);
        uint32_t hi = static_cast<uint16_t>(src[1u]);
        return lo | (hi << 16);
    };

    std::thread producer([&]() {
        std::vector<int16_t> buf(kChunk * kSamplesPerFrame, 0);
        uint64_t next = 0ull;
        while (next < kTotalFrames) {
            uint32_t want = static_cast<uint32_t>(
                std::min<uint64_t>(kChunk, kTotalFrames - next));
            for (uint32_t i = 0u; i < want; ++i) {
                encode(static_cast<uint32_t>(next + i), &buf[i * kSamplesPerFrame]);
            }
            uint32_t wrote = womic_ring_write(&ring, buf.data(), want);
            if (wrote == 0u) {
                std::this_thread::yield();
                continue;
            }
            if (wrote > want) {
                producer_errors.fetch_add(1u);
                break;
            }
            next += wrote;
        }
    });

    std::thread consumer([&]() {
        std::vector<int16_t> buf(kChunk * kSamplesPerFrame, 0);
        uint64_t expect = 0ull;
        while (expect < kTotalFrames) {
            uint32_t got = womic_ring_read(&ring, buf.data(), kChunk);
            if (got == 0u) {
                std::this_thread::yield();
                continue;
            }
            for (uint32_t i = 0u; i < got; ++i) {
                uint32_t v = decode(&buf[i * kSamplesPerFrame]);
                if (v != static_cast<uint32_t>(expect + i)) {
                    /* 严格按序 + 无撕裂：任何一个不匹配都是致命错误 */
                    if (consumer_errors.fetch_add(1u) < 8u) {
                        std::fprintf(stderr,
                                     "  [撕裂/乱序] 期望 %llu 实际 %u\n",
                                     static_cast<unsigned long long>(expect + i), v);
                    }
                }
            }
            expect += got;
        }
        consumed.store(expect);
    });

    producer.join();
    consumer.join();

    WOMIC_CHECK_EQ(producer_errors.load(), 0u);
    WOMIC_CHECK_EQ(consumer_errors.load(), 0u); /* 丢样本 = 0、严格按序、无撕裂 */
    WOMIC_CHECK_EQ(consumed.load(), kTotalFrames);
    WOMIC_CHECK_EQ(ring.written_frames, kTotalFrames);
    WOMIC_CHECK_EQ(ring.read_frames, kTotalFrames);
    WOMIC_CHECK_EQ(ring.dropped_frames, 0u);
    WOMIC_CHECK_EQ(womic_ring_available(&ring), 0u);

    std::printf("      并发 SPSC：%llu 帧，无丢失 / 无乱序 / 无撕裂\n",
                static_cast<unsigned long long>(kTotalFrames));
}

}  // namespace

WOMIC_TEST_MAIN(test_ring_buffer, ({
    test_init_and_basic();
    test_write_read_roundtrip();
    test_empty_and_full();
    test_wrap_around_and_frame_alignment();
    test_read_fill_silence();
    test_reset();
    test_concurrent_stress();
}))
