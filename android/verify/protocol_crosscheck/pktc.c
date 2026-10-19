/*
 * pktc.c -- 用**真实的** shared/core/src/protocol.c 编出一个参考 UDP 包并打印十六进制。
 *
 * 用途：与 PacketizerProbe.java（Packetizer.kt 的逻辑镜像）的输出做逐字节比对，
 * 证明 Android 端封出来的字节流与服务端解码器期望的完全一致。
 *
 * 这不是"另一个实现"，而是直接调用唯一的权威实现。
 */
#include <stdio.h>
#include <stdint.h>

#include "womic/womic_protocol.h"

int main(void) {
    uint8_t payload[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    womic_packet_header h;
    uint8_t dst[128];
    int n;
    int i;
    womic_packet_header out;
    const uint8_t *p = NULL;
    int consumed;

    h.magic = WOMIC_PROTOCOL_MAGIC;
    h.version = WOMIC_PROTOCOL_VERSION;
    h.header_size = WOMIC_PACKET_HEADER_SIZE;
    h.seq = 0x12345678u;
    h.timestamp_ms = 0x01020304u;
    h.sample_rate = 48000u;
    h.channels = 1u;
    h.frames_per_packet = 960u;
    h.payload_bytes = 5u;
    h.flags = WOMIC_FLAG_FEC;
    h.reserved = 0u;

    n = womic_packet_encode(&h, payload, dst, sizeof(dst));
    if (n < 0) {
        printf("encode failed: %d\n", n);
        return 1;
    }

    printf("len=%d\n", n);
    for (i = 0; i < n; ++i) {
        printf("%02X", dst[i]);
    }
    printf("\n");

    /* 用权威解码器回读，确认自洽（服务端 decode 必须接受） */
    consumed = womic_packet_decode(dst, (size_t)n, &out, &p);
    printf("decode=%d seq=%u ts=%u rate=%u ch=%u frames=%u payload=%u flags=%u\n",
           consumed, out.seq, out.timestamp_ms, out.sample_rate, out.channels,
           out.frames_per_packet, out.payload_bytes, out.flags);
    if (consumed != n) {
        printf("DECODE-MISMATCH\n");
        return 1;
    }
    return 0;
}
