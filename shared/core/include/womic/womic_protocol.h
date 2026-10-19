/*
 * womic_protocol.h -- 手机 -> 服务端 UDP 包协议（网络字节序 / 大端）。
 *
 * 不依赖 arpa/inet.h：内核态没有该头文件，因此自带 womic_htons/htonl/ntohs/ntohl。
 * 头部固定 32 字节，所有字段定宽且自然对齐，无填充、无指针，
 * 32/64 位与内核/用户态布局完全一致；header_size 字段保证后续可扩展。
 *
 * 安全要求：decode 必须能承受任意畸形输入（随机长度 / 随机内容 / 篡改长度），
 * 所有越界路径都要被拦住，fuzz 不得崩溃或越界读。
 */
#ifndef WOMIC_PROTOCOL_H
#define WOMIC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WOMIC_PROTOCOL_MAGIC   0x574F4D50u  /* 'WOMP' */
#define WOMIC_PROTOCOL_VERSION 1

#define WOMIC_PACKET_HEADER_SIZE   32u    /* sizeof(womic_packet_header) */
#define WOMIC_PROTO_MAX_PAYLOAD    4096u  /* 单包 Opus payload 上限（远大于 MTU） */
#define WOMIC_PROTO_MAX_HEADER     256u   /* 扩展头部上限，防止 header_size 被篡改成巨大值 */

/* 错误码（encode/decode 返回负值）*/
#define WOMIC_PROTO_ERR_TOO_SHORT   (-1)  /* 缓冲区不足一个头部 */
#define WOMIC_PROTO_ERR_BAD_MAGIC   (-2)  /* magic 不匹配 */
#define WOMIC_PROTO_ERR_BAD_VERSION (-3)  /* 版本号不支持 */
#define WOMIC_PROTO_ERR_BAD_LEN     (-4)  /* header_size / payload_bytes 与 buf_len 不自洽 */
#define WOMIC_PROTO_ERR_NULL_ARG    (-5)  /* 扩展码：空指针参数 */

/* flags 位定义 */
#define WOMIC_FLAG_FEC 0x0001u  /* bit0: 该 payload 带前向纠错（带内 FEC） */
#define WOMIC_FLAG_DTX 0x0002u  /* bit1: 该包为 DTX（静音/舒适噪声）包 */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;       /* = sizeof(womic_packet_header)，保证后续可扩展 */
    uint32_t seq;               /* 每帧 +1，用于 jitter buffer 排序与丢包检测 */
    uint32_t timestamp_ms;      /* 采集侧单调时间戳 */
    uint32_t sample_rate;       /* 16000 或 48000 */
    uint16_t channels;          /* 1 */
    uint16_t frames_per_packet; /* 每包 PCM 帧数（解码后），如 480 / 960 */
    uint16_t payload_bytes;     /* Opus payload 长度 */
    uint16_t flags;             /* bit0: FEC, bit1: DTX */
    uint32_t reserved;
} womic_packet_header;

/* 自写字节序转换（内核态可用，无 libc 依赖，无对齐要求）*/
uint16_t womic_htons(uint16_t v);
uint32_t womic_htonl(uint32_t v);
uint16_t womic_ntohs(uint16_t v);
uint32_t womic_ntohl(uint32_t v);

/**
 * @brief 序列化一个数据包。
 *
 * @param h        头部；header_size 若为 0 则按 32 处理，若 >= 32 则按该值
 *                 （大于 32 的部分填 0，payload 起始偏移相应后移，便于未来扩展）。
 * @param payload  Opus 负载，payload_bytes > 0 时不可为 NULL。
 * @param dst      输出缓冲区。
 * @param dst_len  dst 容量（字节）。必须 >= header_size + payload_bytes 才写。
 * @return 成功返回写入总字节数（header_size + payload_bytes）；
 *         失败返回负的 -WOMIC_PROTO_ERR_*。
 */
int womic_packet_encode(const womic_packet_header *h, const uint8_t *payload,
                        uint8_t *dst, size_t dst_len);

/**
 * @brief 反序列化。严格校验 magic / version / header_size / payload_bytes 与
 *        buf_len 完全一致后才算成功。
 *
 * @param buf         输入缓冲。
 * @param buf_len     输入长度。
 * @param out_h       输出头部，不可为 NULL。
 * @param out_payload 输出 payload 指针（可为 NULL）；payload_bytes == 0 时置 NULL。
 * @return 成功返回 consumed 总字节数（header_size + payload_bytes）；
 *         失败返回负的 -WOMIC_PROTO_ERR_*。
 */
int womic_packet_decode(const uint8_t *buf, size_t buf_len,
                        womic_packet_header *out_h, const uint8_t **out_payload);

#ifdef __cplusplus
}
#endif

#endif /* WOMIC_PROTOCOL_H */
