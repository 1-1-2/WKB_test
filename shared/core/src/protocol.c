/*
 * protocol.c -- UDP 包协议的序列化 / 反序列化。
 *
 * 网络字节序（大端）由本文件自带的 womic_hton* / womic_ntoh* 完成：
 * 内核态没有 arpa/inet.h，也不允许依赖 libc。
 *
 * 安全：所有字段都通过逐字节组装/拆解，不假设输入缓冲区对齐；
 * 所有长度在使用前都做过上界校验，任意畸形输入都只会返回负错误码，
 * 绝不产生越界读（fuzz 目标在 ASan+UBSan 下验证）。
 */
#include "womic/womic_protocol.h"

/* ------------------------------------------------------------------ *
 * 字节序（逐字节实现，无对齐要求，大小端通吃）
 * ------------------------------------------------------------------ */

uint16_t womic_htons(uint16_t v)
{
    return (uint16_t)(((uint16_t)((v & 0x00FFu) << 8)) | ((uint16_t)((v & 0xFF00u) >> 8)));
}

uint32_t womic_htonl(uint32_t v)
{
    return (((v & 0x000000FFu) << 24) |
            ((v & 0x0000FF00u) << 8)  |
            ((v & 0x00FF0000u) >> 8)  |
            ((v & 0xFF000000u) >> 24));
}

uint16_t womic_ntohs(uint16_t v)
{
    return womic_htons(v);
}

uint32_t womic_ntohl(uint32_t v)
{
    return womic_htonl(v);
}

/* ------------------------------------------------------------------ *
 * 内部：逐字节读写（避免未对齐访问，内核与 ASan 均安全）
 * ------------------------------------------------------------------ */

/*
 * 注意：这里直接取“值本身”的高位字节写到低地址，即大端（网络序）布局。
 * 不能再套一层 womic_hton*：hton* 返回的是“在小端主机上按整数加载后
 * 能得到网络序字节序列”的那个整数值，若再按 (be >> 24) 取高位写出去，
 * 等于又交换了一次，结果会变成小端。
 */
static void womic_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void womic_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint16_t womic_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t womic_get_be32(const uint8_t *p)
{
    return (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
            ((uint32_t)p[2] << 8)  | (uint32_t)p[3]);
}

/* ------------------------------------------------------------------ *
 * encode
 * ------------------------------------------------------------------ */

int womic_packet_encode(const womic_packet_header *h, const uint8_t *payload,
                        uint8_t *dst, size_t dst_len)
{
    uint32_t header_size;
    size_t total;
    size_t i;

    if ((h == NULL) || (dst == NULL)) {
        return WOMIC_PROTO_ERR_NULL_ARG;
    }
    if (h->payload_bytes > WOMIC_PROTO_MAX_PAYLOAD) {
        return WOMIC_PROTO_ERR_BAD_LEN;
    }
    if ((h->payload_bytes > 0u) && (payload == NULL)) {
        return WOMIC_PROTO_ERR_NULL_ARG;
    }

    /* header_size：0 或未达最小头部时按标准 32 处理；允许扩展（>32 的部分填 0） */
    if ((h->header_size == 0u) || (h->header_size < WOMIC_PACKET_HEADER_SIZE)) {
        header_size = WOMIC_PACKET_HEADER_SIZE;
    } else {
        header_size = h->header_size;
        if (header_size > WOMIC_PROTO_MAX_HEADER) {
            return WOMIC_PROTO_ERR_BAD_LEN;
        }
    }

    total = (size_t)header_size + (size_t)h->payload_bytes;
    if (dst_len < total) {
        return WOMIC_PROTO_ERR_TOO_SHORT;
    }

    womic_put_be32(&dst[0], h->magic);
    womic_put_be16(&dst[4], h->version);
    womic_put_be16(&dst[6], (uint16_t)header_size);
    womic_put_be32(&dst[8], h->seq);
    womic_put_be32(&dst[12], h->timestamp_ms);
    womic_put_be32(&dst[16], h->sample_rate);
    womic_put_be16(&dst[20], h->channels);
    womic_put_be16(&dst[22], h->frames_per_packet);
    womic_put_be16(&dst[24], h->payload_bytes);
    womic_put_be16(&dst[26], h->flags);
    womic_put_be32(&dst[28], h->reserved);

    /* 扩展头部区域填 0 */
    for (i = WOMIC_PACKET_HEADER_SIZE; i < (size_t)header_size; ++i) {
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

int womic_packet_decode(const uint8_t *buf, size_t buf_len,
                        womic_packet_header *out_h, const uint8_t **out_payload)
{
    uint32_t magic;
    uint16_t version;
    uint32_t header_size;
    uint16_t payload_bytes;
    size_t total;

    if ((buf == NULL) || (out_h == NULL)) {
        return WOMIC_PROTO_ERR_NULL_ARG;
    }
    if (buf_len < (size_t)WOMIC_PACKET_HEADER_SIZE) {
        return WOMIC_PROTO_ERR_TOO_SHORT;
    }

    /* 先做 magic 校验，最快地拒绝随机垃圾包 */
    magic = womic_get_be32(&buf[0]);
    if (magic != WOMIC_PROTOCOL_MAGIC) {
        return WOMIC_PROTO_ERR_BAD_MAGIC;
    }
    version = womic_get_be16(&buf[4]);
    if (version != (uint16_t)WOMIC_PROTOCOL_VERSION) {
        return WOMIC_PROTO_ERR_BAD_VERSION;
    }

    header_size = (uint32_t)womic_get_be16(&buf[6]);
    if ((header_size < WOMIC_PACKET_HEADER_SIZE) || (header_size > WOMIC_PROTO_MAX_HEADER)) {
        return WOMIC_PROTO_ERR_BAD_LEN;
    }
    payload_bytes = womic_get_be16(&buf[24]);
    if ((uint32_t)payload_bytes > WOMIC_PROTO_MAX_PAYLOAD) {
        return WOMIC_PROTO_ERR_BAD_LEN;
    }

    /* 严格自洽性：buf_len 必须恰好等于 header_size + payload_bytes */
    total = (size_t)header_size + (size_t)payload_bytes;
    if (buf_len < (size_t)header_size) {
        return WOMIC_PROTO_ERR_BAD_LEN;
    }
    if (buf_len != total) {
        return WOMIC_PROTO_ERR_BAD_LEN;
    }

    out_h->magic = magic;
    out_h->version = version;
    out_h->header_size = (uint16_t)header_size;
    out_h->seq = womic_get_be32(&buf[8]);
    out_h->timestamp_ms = womic_get_be32(&buf[12]);
    out_h->sample_rate = womic_get_be32(&buf[16]);
    out_h->channels = womic_get_be16(&buf[20]);
    out_h->frames_per_packet = womic_get_be16(&buf[22]);
    out_h->payload_bytes = payload_bytes;
    out_h->flags = womic_get_be16(&buf[26]);
    out_h->reserved = womic_get_be32(&buf[28]);

    if (out_payload != NULL) {
        if (payload_bytes == 0u) {
            *out_payload = NULL;
        } else {
            *out_payload = &buf[header_size];
        }
    }

    return (int)total;
}
