/*
 * protocol.c -- UDP 包协议的序列化 / 反序列化。
 *
 * 网络字节序（大端）由本文件自带的 miclink_hton* / miclink_ntoh* 完成：
 * 内核态没有 arpa/inet.h，也不允许依赖 libc。
 *
 * 安全：所有字段都通过逐字节组装/拆解，不假设输入缓冲区对齐；
 * 所有长度在使用前都做过上界校验，任意畸形输入都只会返回负错误码，
 * 绝不产生越界读（fuzz 目标在 ASan+UBSan 下验证）。
 */
#include "miclink/miclink_protocol.h"

/* ------------------------------------------------------------------ *
 * 字节序（逐字节实现，无对齐要求，大小端通吃）
 * ------------------------------------------------------------------ */

uint16_t miclink_htons(uint16_t v)
{
    return (uint16_t)(((uint16_t)((v & 0x00FFu) << 8)) | ((uint16_t)((v & 0xFF00u) >> 8)));
}

uint32_t miclink_htonl(uint32_t v)
{
    return (((v & 0x000000FFu) << 24) |
            ((v & 0x0000FF00u) << 8)  |
            ((v & 0x00FF0000u) >> 8)  |
            ((v & 0xFF000000u) >> 24));
}

uint16_t miclink_ntohs(uint16_t v)
{
    return miclink_htons(v);
}

uint32_t miclink_ntohl(uint32_t v)
{
    return miclink_htonl(v);
}

/* ------------------------------------------------------------------ *
 * 内部：逐字节读写（避免未对齐访问，内核与 ASan 均安全）
 * ------------------------------------------------------------------ */

/*
 * 注意：这里直接取“值本身”的高位字节写到低地址，即大端（网络序）布局。
 * 不能再套一层 miclink_hton*：hton* 返回的是“在小端主机上按整数加载后
 * 能得到网络序字节序列”的那个整数值，若再按 (be >> 24) 取高位写出去，
 * 等于又交换了一次，结果会变成小端。
 */
static void miclink_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void miclink_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint16_t miclink_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t miclink_get_be32(const uint8_t *p)
{
    return (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
            ((uint32_t)p[2] << 8)  | (uint32_t)p[3]);
}

/* ------------------------------------------------------------------ *
 * encode
 * ------------------------------------------------------------------ */

int miclink_packet_encode(const miclink_packet_header *h, const uint8_t *payload,
                        uint8_t *dst, size_t dst_len)
{
    uint32_t header_size;
    size_t total;
    size_t i;

    if ((h == NULL) || (dst == NULL)) {
        return MICLINK_PROTO_ERR_NULL_ARG;
    }
    if (h->payload_bytes > MICLINK_PROTO_MAX_PAYLOAD) {
        return MICLINK_PROTO_ERR_BAD_LEN;
    }
    if ((h->payload_bytes > 0u) && (payload == NULL)) {
        return MICLINK_PROTO_ERR_NULL_ARG;
    }

    /* header_size：0 或未达最小头部时按标准 32 处理；允许扩展（>32 的部分填 0） */
    if ((h->header_size == 0u) || (h->header_size < MICLINK_PACKET_HEADER_SIZE)) {
        header_size = MICLINK_PACKET_HEADER_SIZE;
    } else {
        header_size = h->header_size;
        if (header_size > MICLINK_PROTO_MAX_HEADER) {
            return MICLINK_PROTO_ERR_BAD_LEN;
        }
    }

    total = (size_t)header_size + (size_t)h->payload_bytes;
    if (dst_len < total) {
        return MICLINK_PROTO_ERR_TOO_SHORT;
    }

    miclink_put_be32(&dst[0], h->magic);
    miclink_put_be16(&dst[4], h->version);
    miclink_put_be16(&dst[6], (uint16_t)header_size);
    miclink_put_be32(&dst[8], h->seq);
    miclink_put_be32(&dst[12], h->timestamp_ms);
    miclink_put_be32(&dst[16], h->sample_rate);
    miclink_put_be16(&dst[20], h->channels);
    miclink_put_be16(&dst[22], h->frames_per_packet);
    miclink_put_be16(&dst[24], h->payload_bytes);
    miclink_put_be16(&dst[26], h->flags);
    miclink_put_be32(&dst[28], h->reserved);

    /* 扩展头部区域填 0 */
    for (i = MICLINK_PACKET_HEADER_SIZE; i < (size_t)header_size; ++i) {
        dst[i] = 0u;
    }
    /* payload */
    for (i = 0u; i < (size_t)h->payload_bytes; ++i) {
        dst[(size_t)header_size + i] = payload[i];
    }

    return (int)total;
}

/* ------------------------------------------------------------------ *
 * decode
 * ------------------------------------------------------------------ */

int miclink_packet_decode(const uint8_t *buf, size_t buf_len,
                        miclink_packet_header *out_h, const uint8_t **out_payload)
{
    uint32_t magic;
    uint16_t version;
    uint32_t header_size;
    uint16_t payload_bytes;
    size_t total;

    if ((buf == NULL) || (out_h == NULL)) {
        return MICLINK_PROTO_ERR_NULL_ARG;
    }
    if (buf_len < (size_t)MICLINK_PACKET_HEADER_SIZE) {
        return MICLINK_PROTO_ERR_TOO_SHORT;
    }

    /* 先做 magic 校验，最快地拒绝随机垃圾包 */
    magic = miclink_get_be32(&buf[0]);
    if (magic != MICLINK_PROTOCOL_MAGIC) {
        return MICLINK_PROTO_ERR_BAD_MAGIC;
    }
    version = miclink_get_be16(&buf[4]);
    if (version != (uint16_t)MICLINK_PROTOCOL_VERSION) {
        return MICLINK_PROTO_ERR_BAD_VERSION;
    }

    header_size = (uint32_t)miclink_get_be16(&buf[6]);
    if ((header_size < MICLINK_PACKET_HEADER_SIZE) || (header_size > MICLINK_PROTO_MAX_HEADER)) {
        return MICLINK_PROTO_ERR_BAD_LEN;
    }
    payload_bytes = miclink_get_be16(&buf[24]);
    if ((uint32_t)payload_bytes > MICLINK_PROTO_MAX_PAYLOAD) {
        return MICLINK_PROTO_ERR_BAD_LEN;
    }

    /* 严格自洽性：buf_len 必须恰好等于 header_size + payload_bytes */
    total = (size_t)header_size + (size_t)payload_bytes;
    if (buf_len < (size_t)header_size) {
        return MICLINK_PROTO_ERR_BAD_LEN;
    }
    if (buf_len != total) {
        return MICLINK_PROTO_ERR_BAD_LEN;
    }

    out_h->magic = magic;
    out_h->version = version;
    out_h->header_size = (uint16_t)header_size;
    out_h->seq = miclink_get_be32(&buf[8]);
    out_h->timestamp_ms = miclink_get_be32(&buf[12]);
    out_h->sample_rate = miclink_get_be32(&buf[16]);
    out_h->channels = miclink_get_be16(&buf[20]);
    out_h->frames_per_packet = miclink_get_be16(&buf[22]);
    out_h->payload_bytes = payload_bytes;
    out_h->flags = miclink_get_be16(&buf[26]);
    out_h->reserved = miclink_get_be32(&buf[28]);

    if (out_payload != NULL) {
        if (payload_bytes == 0u) {
            *out_payload = NULL;
        } else {
            *out_payload = &buf[header_size];
        }
    }

    return (int)total;
}
