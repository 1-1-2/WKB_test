/*
 * test_protocol.cpp -- UDP 包协议测试：
 *   - 字节序工具函数
 *   - 包头往返（encode -> decode 字段完全一致）
 *   - 扩展头部（header_size > 32）
 *   - 边界长度（恰好够 / 少 1 字节 / 空缓冲）
 *   - 错误码（TOO_SHORT / BAD_MAGIC / BAD_VERSION / BAD_LEN / NULL_ARG）
 */
#include <womic/womic_ioctl.h>
#include <womic/womic_protocol.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "test_util.h"

namespace {

/*
 * IOCTL 契约是驱动 / 服务端 / 单测三方共用的唯一事实来源，
 * 这里把“控制码编码”和“结构体布局”钉死：任何一处被改动都会立刻编译或断言失败。
 */
void test_ioctl_contract()
{
    /* 控制码编码：DeviceType<<16 | Access<<14 | Function<<2 | Method */
    WOMIC_CHECK_EQ(WOMIC_IOCTL_GET_VERSION, 0x00222400u);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_GET_CAPS, 0x00222404u);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_GET_FORMAT, 0x00222408u);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_SET_FORMAT, 0x0022640Cu); /* FILE_WRITE_ACCESS */
    WOMIC_CHECK_EQ(WOMIC_IOCTL_WRITE_PCM, 0x00226410u);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_GET_STATUS, 0x00222414u);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_RESET, 0x00226418u);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_START, 0x0022641Cu);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_STOP, 0x00226420u);
    WOMIC_CHECK_EQ(WOMIC_IOCTL_DEVICE_TYPE, 0x00000022u);

    /* 结构体布局（32/64 位、MSVC/GCC 必须一致） */
    WOMIC_CHECK_EQ(sizeof(WOMIC_VERSION), 16u);
    WOMIC_CHECK_EQ(sizeof(WOMIC_FORMAT), 12u);
    WOMIC_CHECK_EQ(sizeof(WOMIC_CAPS), 48u);
    WOMIC_CHECK_EQ(sizeof(WOMIC_WRITE_PCM_IO), 16u);
    WOMIC_CHECK_EQ(sizeof(WOMIC_RESET_IO), 16u);
    WOMIC_CHECK_EQ(sizeof(WOMIC_START_IO), 16u);
    WOMIC_CHECK_EQ(sizeof(WOMIC_STATUS), 64u);

    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, state), 0u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, ringAvailableFrames), 4u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, ringFreeFrames), 8u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, underrunCount), 12u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, framesWritten), 16u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, framesConsumed), 24u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, droppedFrames), 32u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, overrunCount), 40u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, ringHighWatermark), 44u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_STATUS, currentFormat), 48u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_FORMAT, sampleRate), 0u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_FORMAT, channels), 4u);
    WOMIC_CHECK_EQ(offsetof(WOMIC_FORMAT, bitsPerSample), 8u);

    /* 错误码与状态常量 */
    WOMIC_CHECK_EQ(WOMIC_OK, 0u);
    WOMIC_CHECK_EQ(WOMIC_ERR_NOT_READY, 1u);
    WOMIC_CHECK_EQ(WOMIC_ERR_BUFFER_FULL, 2u);
    WOMIC_CHECK_EQ(WOMIC_ERR_FORMAT_MISMATCH, 3u);
    WOMIC_CHECK_EQ(WOMIC_ERR_INVALID_PARAM, 4u);
    WOMIC_CHECK_EQ(WOMIC_ERR_NO_STREAM, 5u);
    WOMIC_CHECK_EQ(WOMIC_STATE_STOPPED, 0u);
    WOMIC_CHECK_EQ(WOMIC_STATE_RUNNING, 2u);
}

void test_byte_order()
{
    WOMIC_CHECK_EQ(womic_htons(0x1234u), 0x3412u);
    WOMIC_CHECK_EQ(womic_htonl(0x01020304u), 0x04030201u);
    WOMIC_CHECK_EQ(womic_ntohs(womic_htons(0xABCDu)), 0xABCDu);
    WOMIC_CHECK_EQ(womic_ntohl(womic_htonl(0x89ABCDEFu)), 0x89ABCDEFu);
    WOMIC_CHECK_EQ(womic_htons(0u), 0u);
    WOMIC_CHECK_EQ(womic_htonl(0xFFFFFFFFu), 0xFFFFFFFFu);
}

void test_header_layout()
{
    /* 头部必须是 32 字节且字段无填充 */
    WOMIC_CHECK_EQ(sizeof(womic_packet_header), 32u);
    womic_packet_header h;
    WOMIC_CHECK_EQ((size_t)((const uint8_t *)&h.reserved - (const uint8_t *)&h), 28u);
}

void test_roundtrip()
{
    womic_packet_header h;
    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = (uint16_t)sizeof(womic_packet_header);
    h.seq = 0xDEADBEEFu;
    h.timestamp_ms = 123456789u;
    h.sample_rate = 48000u;
    h.channels = 1u;
    h.frames_per_packet = 960u;
    h.payload_bytes = 120u;
    h.flags = WOMIC_FLAG_FEC;
    h.reserved = 0x11223344u;

    std::vector<uint8_t> payload(h.payload_bytes);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i * 7u + 3u);
    }

    std::vector<uint8_t> buf(sizeof(womic_packet_header) + payload.size(), 0u);
    int n = womic_packet_encode(&h, payload.data(), buf.data(), buf.size());
    WOMIC_CHECK_EQ(n, (int)buf.size());

    /* 网络字节序（大端）落盘验证 */
    WOMIC_CHECK_EQ(buf[0], 0x57u); /* 'W' */
    WOMIC_CHECK_EQ(buf[1], 0x4Fu); /* 'O' */
    WOMIC_CHECK_EQ(buf[2], 0x4Du); /* 'M' */
    WOMIC_CHECK_EQ(buf[3], 0x50u); /* 'P' */
    WOMIC_CHECK_EQ(buf[4], 0x00u); /* version 高字节 */
    WOMIC_CHECK_EQ(buf[5], 0x01u); /* version 低字节 */
    WOMIC_CHECK_EQ(buf[6], 0x00u); /* header_size 高字节 */
    WOMIC_CHECK_EQ(buf[7], 0x20u); /* header_size = 32 */

    womic_packet_header out;
    const uint8_t *pl = nullptr;
    int rc = womic_packet_decode(buf.data(), buf.size(), &out, &pl);
    WOMIC_CHECK_EQ(rc, (int)buf.size());
    WOMIC_CHECK_EQ(out.magic, WOMIC_PROTOCOL_MAGIC);
    WOMIC_CHECK_EQ(out.version, WOMIC_PROTOCOL_VERSION);
    WOMIC_CHECK_EQ(out.header_size, 32u);
    WOMIC_CHECK_EQ(out.seq, 0xDEADBEEFu);
    WOMIC_CHECK_EQ(out.timestamp_ms, 123456789u);
    WOMIC_CHECK_EQ(out.sample_rate, 48000u);
    WOMIC_CHECK_EQ(out.channels, 1u);
    WOMIC_CHECK_EQ(out.frames_per_packet, 960u);
    WOMIC_CHECK_EQ(out.payload_bytes, 120u);
    WOMIC_CHECK_EQ(out.flags, WOMIC_FLAG_FEC);
    WOMIC_CHECK_EQ(out.reserved, 0x11223344u);
    WOMIC_CHECK(pl == buf.data() + 32u);
    for (size_t i = 0; i < payload.size(); ++i) {
        WOMIC_CHECK_EQ(pl[i], payload[i]);
    }
}

void test_zero_payload()
{
    womic_packet_header h;
    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = 32u;
    h.seq = 1u;
    h.timestamp_ms = 0u;
    h.sample_rate = 16000u;
    h.channels = 1u;
    h.frames_per_packet = 160u;
    h.payload_bytes = 0u;
    h.flags = WOMIC_FLAG_DTX;
    h.reserved = 0u;

    std::vector<uint8_t> buf(32u, 0u);
    WOMIC_CHECK_EQ(womic_packet_encode(&h, nullptr, buf.data(), buf.size()), 32);

    womic_packet_header out;
    const uint8_t *pl = reinterpret_cast<const uint8_t *>(1);
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), buf.size(), &out, &pl), 32);
    WOMIC_CHECK_EQ(out.payload_bytes, 0u);
    WOMIC_CHECK(pl == nullptr);
}

void test_extended_header()
{
    womic_packet_header h;
    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = 48u; /* 扩展 16 字节 */
    h.seq = 42u;
    h.timestamp_ms = 7u;
    h.sample_rate = 48000u;
    h.channels = 2u;
    h.frames_per_packet = 480u;
    h.payload_bytes = 8u;
    h.flags = 0u;
    h.reserved = 0u;

    std::vector<uint8_t> payload(8u, 0xABu);
    std::vector<uint8_t> buf(48u + 8u, 0xFFu);
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), buf.data(), buf.size()), (int)buf.size());
    /* 扩展区必须被清零 */
    for (size_t i = 32u; i < 48u; ++i) {
        WOMIC_CHECK_EQ(buf[i], 0u);
    }

    womic_packet_header out;
    const uint8_t *pl = nullptr;
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), buf.size(), &out, &pl), (int)buf.size());
    WOMIC_CHECK_EQ(out.header_size, 48u);
    WOMIC_CHECK(pl == buf.data() + 48u);
}

void test_boundary_lengths()
{
    womic_packet_header h;
    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = 32u;
    h.seq = 5u;
    h.timestamp_ms = 9u;
    h.sample_rate = 48000u;
    h.channels = 1u;
    h.frames_per_packet = 960u;
    h.payload_bytes = 300u;
    h.flags = 0u;
    h.reserved = 0u;

    std::vector<uint8_t> payload(300u, 0x5Au);
    const size_t need = 32u + 300u;

    /* dst_len 少 1 字节 -> TOO_SHORT，且绝不写越界 */
    std::vector<uint8_t> buf(need + 64u, 0xEEu);
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), buf.data(), need - 1u),
                   WOMIC_PROTO_ERR_TOO_SHORT);
    for (size_t i = 0; i < buf.size(); ++i) {
        WOMIC_CHECK_EQ(buf[i], 0xEEu); /* 失败时不允许写任何字节 */
    }
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), buf.data(), need), (int)need);
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), buf.data(), need + 64u), (int)need);

    /* decode 边界：31 字节 -> TOO_SHORT；0 长度 -> TOO_SHORT */
    womic_packet_header out;
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), 31u, &out, nullptr), WOMIC_PROTO_ERR_TOO_SHORT);
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), 0u, &out, nullptr), WOMIC_PROTO_ERR_TOO_SHORT);
    WOMIC_CHECK_EQ(womic_packet_decode(nullptr, 0u, &out, nullptr), WOMIC_PROTO_ERR_NULL_ARG);
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), 64u, nullptr, nullptr), WOMIC_PROTO_ERR_NULL_ARG);

    /* 长度少 1 / 多 1 -> BAD_LEN */
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), need - 1u, &out, nullptr), WOMIC_PROTO_ERR_BAD_LEN);
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), need + 1u, &out, nullptr), WOMIC_PROTO_ERR_BAD_LEN);
}

void test_error_codes()
{
    womic_packet_header h;
    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = 32u;
    h.seq = 1u;
    h.timestamp_ms = 1u;
    h.sample_rate = 48000u;
    h.channels = 1u;
    h.frames_per_packet = 960u;
    h.payload_bytes = 16u;
    h.flags = 0u;
    h.reserved = 0u;

    std::vector<uint8_t> payload(16u, 1u);
    std::vector<uint8_t> buf(48u, 0u);
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), buf.data(), buf.size()), 48);

    womic_packet_header out;
    const uint8_t *pl = nullptr;

    /* BAD_MAGIC */
    buf[0] = 0x00u;
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), buf.size(), &out, &pl), WOMIC_PROTO_ERR_BAD_MAGIC);
    buf[0] = 0x57u;

    /* BAD_VERSION */
    buf[5] = 0x02u;
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), buf.size(), &out, &pl), WOMIC_PROTO_ERR_BAD_VERSION);
    buf[5] = 0x01u;

    /* BAD_LEN：header_size 被篡改为 0 / 20 / 999 */
    uint16_t hs = static_cast<uint16_t>((buf[6] << 8) | buf[7]);
    WOMIC_CHECK_EQ(hs, 32u);
    buf[7] = 20u; /* header_size = 20 < 32 */
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), buf.size(), &out, &pl), WOMIC_PROTO_ERR_BAD_LEN);
    buf[7] = 32u;

    /* BAD_LEN：payload_bytes 被篡改（与 buf_len 不自洽） */
    buf[24] = 0x00u;
    buf[25] = 0x11u; /* payload_bytes = 17，但 buf_len = 48 -> 32+17=49 != 48 */
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), buf.size(), &out, &pl), WOMIC_PROTO_ERR_BAD_LEN);
    buf[24] = 0x00u;
    buf[25] = 0x10u;

    /* 恢复正常 */
    WOMIC_CHECK_EQ(womic_packet_decode(buf.data(), buf.size(), &out, &pl), 48);

    /* payload 超限 -> BAD_LEN */
    h.payload_bytes = WOMIC_PROTO_MAX_PAYLOAD + 1u;
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), buf.data(), buf.size()),
                   WOMIC_PROTO_ERR_BAD_LEN);
    h.payload_bytes = 16u;

    /* header_size 超限 -> BAD_LEN */
    h.header_size = WOMIC_PROTO_MAX_HEADER + 1u;
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), buf.data(), buf.size()),
                   WOMIC_PROTO_ERR_BAD_LEN);
    h.header_size = 32u;

    /* payload_bytes > 0 但 payload == NULL -> NULL_ARG */
    WOMIC_CHECK_EQ(womic_packet_encode(&h, nullptr, buf.data(), buf.size()),
                   WOMIC_PROTO_ERR_NULL_ARG);
    /* dst == NULL -> NULL_ARG */
    WOMIC_CHECK_EQ(womic_packet_encode(&h, payload.data(), nullptr, 64u),
                   WOMIC_PROTO_ERR_NULL_ARG);
}

}  // namespace

WOMIC_TEST_MAIN(test_protocol, ({
    test_ioctl_contract();
    test_byte_order();
    test_header_layout();
    test_roundtrip();
    test_zero_payload();
    test_extended_header();
    test_boundary_lengths();
    test_error_codes();
}))
