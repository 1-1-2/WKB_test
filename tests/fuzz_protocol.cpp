/*
 * fuzz_protocol.cpp -- 协议解析模糊测试（ASan + UBSan）。
 *
 * 15 万轮，三类输入：
 *   1) 纯随机：长度 0..64，内容全随机；
 *   2) 合法头 + 篡改长度：magic/version 正确，payload_bytes / header_size 被随机改写，
 *      buf_len 也随机（制造“声明长度与实际长度不自洽”的最危险组合）；
 *   3) 合法包 + 随机 dst_len 的 encode 往返（验证 encode 的边界检查与自洽性）。
 *
 * 断言：任何输入都不崩溃、不越界（由 ASan 保证），返回值只可能是
 *       成功（>0）或已知的负错误码；成功时 payload 指针与长度必须落在缓冲区内。
 */
#include <womic/womic_protocol.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "test_util.h"

namespace {

constexpr int kRounds = 150000; /* PRD 要求 >= 10 万轮；实际跑 15 万轮 */

/* 大端写入：直接把值的高位字节写到低地址 */
void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

void fuzz_decode_random(womic_test::Rng &rng)
{
    const uint32_t len = rng.next(65u); /* 0..64 */
    uint8_t *buf = static_cast<uint8_t *>(std::malloc(len > 0u ? len : 1u));
    for (uint32_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(rng.next(256u));
    }

    womic_packet_header h;
    const uint8_t *pl = nullptr;
    const int rc = womic_packet_decode(buf, len, &h, &pl);
    if (rc > 0) {
        WOMIC_CHECK_EQ((size_t)rc, (size_t)len);
        WOMIC_CHECK_EQ((size_t)rc, (size_t)((size_t)h.header_size + (size_t)h.payload_bytes));
        WOMIC_CHECK_EQ(h.magic, WOMIC_PROTOCOL_MAGIC);
        WOMIC_CHECK_EQ(h.version, (uint16_t)WOMIC_PROTOCOL_VERSION);
        if (h.payload_bytes != 0u) {
            WOMIC_CHECK(pl != nullptr);
            WOMIC_CHECK((const uint8_t *)pl >= buf);
            WOMIC_CHECK((const uint8_t *)pl + h.payload_bytes <= buf + len);
        }
    } else {
        WOMIC_CHECK(rc >= WOMIC_PROTO_ERR_NULL_ARG);
        WOMIC_CHECK(rc <= WOMIC_PROTO_ERR_TOO_SHORT);
    }
    std::free(buf);
}

void fuzz_decode_valid_header_tampered_len(womic_test::Rng &rng)
{
    /* 先构造一个自洽的合法包 */
    womic_packet_header h;
    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = 32u;
    h.seq = rng.next_u32();
    h.timestamp_ms = rng.next_u32();
    h.sample_rate = (rng.next(2u) == 0u) ? 16000u : 48000u;
    h.channels = 1u;
    h.frames_per_packet = (uint16_t)(rng.next(2000u));
    h.payload_bytes = (uint16_t)rng.next(64u); /* 0..63 */
    h.flags = (uint16_t)rng.next(4u);
    h.reserved = rng.next_u32();

    std::vector<uint8_t> payload(h.payload_bytes, 0u);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(rng.next(256u));
    }

    /* 缓冲区长度随机（可能与声明长度不一致） */
    const uint32_t alloc_len = 32u + rng.next(80u);
    uint8_t *buf = static_cast<uint8_t *>(std::malloc(alloc_len > 0u ? alloc_len : 1u));
    for (uint32_t i = 0; i < alloc_len; ++i) {
        buf[i] = static_cast<uint8_t>(rng.next(256u));
    }

    /* 覆盖写入合法头 */
    write_be32(&buf[0], h.magic);
    write_be16(&buf[4], h.version);
    write_be16(&buf[6], h.header_size);
    write_be32(&buf[8], h.seq);
    write_be32(&buf[12], h.timestamp_ms);
    write_be32(&buf[16], h.sample_rate);
    write_be16(&buf[20], h.channels);
    write_be16(&buf[22], h.frames_per_packet);
    write_be16(&buf[24], h.payload_bytes);
    write_be16(&buf[26], h.flags);
    write_be32(&buf[28], h.reserved);

    /* 篡改：随机改写 header_size 与 payload_bytes 的字节 */
    if (rng.chance(50u)) {
        buf[7] = static_cast<uint8_t>(rng.next(256u)); /* header_size 低字节 */
    }
    if (rng.chance(50u)) {
        buf[24] = static_cast<uint8_t>(rng.next(256u)); /* payload_bytes 高字节 */
    }
    if (rng.chance(50u)) {
        buf[25] = static_cast<uint8_t>(rng.next(256u)); /* payload_bytes 低字节 */
    }

    womic_packet_header out;
    const uint8_t *pl = nullptr;
    const int rc = womic_packet_decode(buf, alloc_len, &out, &pl);
    if (rc > 0) {
        WOMIC_CHECK_EQ((size_t)rc, (size_t)alloc_len);
        WOMIC_CHECK_EQ((size_t)rc, (size_t)((size_t)out.header_size + (size_t)out.payload_bytes));
        WOMIC_CHECK(out.header_size >= WOMIC_PACKET_HEADER_SIZE);
        if (out.payload_bytes != 0u) {
            WOMIC_CHECK((const uint8_t *)pl >= buf);
            WOMIC_CHECK((const uint8_t *)pl + out.payload_bytes <= buf + alloc_len);
        }
    } else {
        WOMIC_CHECK(rc >= WOMIC_PROTO_ERR_NULL_ARG);
        WOMIC_CHECK(rc <= WOMIC_PROTO_ERR_TOO_SHORT);
    }
    std::free(buf);
}

void fuzz_encode(womic_test::Rng &rng)
{
    womic_packet_header h;
    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = rng.chance(80u) ? 32u : (uint16_t)rng.next((uint32_t)WOMIC_PROTO_MAX_HEADER + 1u);
    h.seq = rng.next_u32();
    h.timestamp_ms = rng.next_u32();
    h.sample_rate = rng.next(2u) == 0u ? 16000u : 48000u;
    h.channels = (uint16_t)(1u + rng.next(2u));
    h.frames_per_packet = (uint16_t)rng.next(2000u);
    /* 偶尔给出超限的 payload_bytes，验证 encode 拒绝 */
    h.payload_bytes = rng.chance(10u)
                          ? (uint16_t)rng.next(0xFFFFu)
                          : (uint16_t)rng.next(300u);
    h.flags = (uint16_t)rng.next(4u);
    h.reserved = rng.next_u32();

    std::vector<uint8_t> payload(h.payload_bytes, 0u);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(rng.next(256u));
    }

    /* dst_len 随机（常常不够） */
    const uint32_t dst_len = rng.next(400u);
    std::vector<uint8_t> dst(dst_len > 0u ? dst_len : 1u, 0x5Au);

    const int rc = womic_packet_encode(&h, payload.data(), dst.data(), dst_len);
    if (rc > 0) {
        /* encode 会把 < 32 的 header_size 规范化为 32（协议最小头部） */
        const size_t eff_header = (h.header_size < WOMIC_PACKET_HEADER_SIZE)
                                      ? (size_t)WOMIC_PACKET_HEADER_SIZE
                                      : (size_t)h.header_size;
        WOMIC_CHECK_EQ((size_t)rc, eff_header + (size_t)h.payload_bytes);
        WOMIC_CHECK((size_t)rc <= (size_t)dst_len);

        /* 自洽性：encode 的结果必须能被 decode 接受 */
        womic_packet_header out;
        const uint8_t *pl = nullptr;
        const int drc = womic_packet_decode(dst.data(), (size_t)rc, &out, &pl);
        WOMIC_CHECK_EQ(drc, rc);
        WOMIC_CHECK_EQ(out.seq, h.seq);
        WOMIC_CHECK_EQ(out.header_size, eff_header);
        WOMIC_CHECK_EQ(out.payload_bytes, h.payload_bytes);
        if (h.payload_bytes != 0u) {
            WOMIC_CHECK(pl != nullptr);
            if (pl != nullptr) {
                WOMIC_CHECK_EQ(std::memcmp(pl, payload.data(), h.payload_bytes), 0);
            }
        }
    } else {
        WOMIC_CHECK(rc >= WOMIC_PROTO_ERR_NULL_ARG);
        WOMIC_CHECK(rc <= WOMIC_PROTO_ERR_TOO_SHORT);
    }
}

}  // namespace

WOMIC_TEST_MAIN(fuzz_protocol, ({
    womic_test::Rng rng(0xC0FFEEu);
    for (int i = 0; i < kRounds; ++i) {
        fuzz_decode_random(rng);
        fuzz_decode_valid_header_tampered_len(rng);
        fuzz_encode(rng);
    }
    std::printf("      fuzz 完成：%d 轮 x 3 类输入\n", kRounds);
}))
