package com.miclink.open

import java.nio.ByteBuffer

/**
 * Packetizer -- 把一帧 Opus payload 封装成 miclink UDP 包。
 *
 * 【唯一事实来源】字段定义、偏移量、字节序全部来自
 * `shared/core/include/miclink/miclink_protocol.h` + `shared/core/src/protocol.c`：
 *
 * | 偏移 | 长度 | 字段                  | 说明                                |
 * |------|------|-----------------------|-------------------------------------|
 * | 0    | 4    | magic                 | 0x574F4D50 ('WOMP')                  |
 * | 4    | 2    | version               | 1                                    |
 * | 6    | 2    | header_size           | 32                                   |
 * | 8    | 4    | seq                   | 每帧 +1                              |
 * | 12   | 4    | timestamp_ms          | 采集侧单调时钟（elapsedRealtime）    |
 * | 16   | 4    | sample_rate           | 16000 / 48000                        |
 * | 20   | 2    | channels              | 1                                    |
 * | 22   | 2    | frames_per_packet     | 解码后每包 PCM 帧数                  |
 * | 24   | 2    | payload_bytes         | Opus payload 长度                    |
 * | 26   | 2    | flags                 | bit0=FEC, bit1=DTX                   |
 * | 28   | 4    | reserved              | 0                                    |
 *
 * 全部**大端（网络字节序）**。header_size 之后紧跟 payload，
 * 服务端 miclink_packet_decode() 要求 `buf_len == header_size + payload_bytes`，
 * 因此本类产出的 ByteArray 长度必须恰好是 32 + payloadLen（不许带尾随 0）。
 */
object Packetizer {

    /** 'WOMP'，与 MICLINK_PROTOCOL_MAGIC 一致。 */
    const val MAGIC: Int = 0x574F_4D50.toInt()

    /** 与 MICLINK_PROTOCOL_VERSION 一致。 */
    const val VERSION: Int = 1

    /** 与 MICLINK_PACKET_HEADER_SIZE 一致（sizeof(miclink_packet_header)）。 */
    const val HEADER_SIZE: Int = 32

    /** 与 MICLINK_PROTO_MAX_PAYLOAD 一致。 */
    const val MAX_PAYLOAD: Int = 4096

    /** flags：该 payload 带前向纠错（本实现中为上一帧的冗余重发包）。 */
    const val FLAG_FEC: Int = 0x0001

    /** flags：该包为 DTX / 舒适噪声包。 */
    const val FLAG_DTX: Int = 0x0002

    /** 包体总上限：头部 + payload（MAX_PAYLOAD）。 */
    const val MAX_PACKET_SIZE: Int = HEADER_SIZE + MAX_PAYLOAD

    /**
     * 把头部字段写入 [dst]（大端），payload 随后追加。
     *
     * @param dst         目标缓冲，容量需 >= HEADER_SIZE + payloadLen
     * @param seq         包序号，每帧 +1（UInt 自然回绕，与 C 端 uint32_t 一致）
     * @param timestampMs 采集侧单调时间戳（SystemClock.elapsedRealtime() 的毫秒数）
     * @param sampleRate  Opus 采样率（16000 / 48000）
     * @param channels    声道数（当前固定 1）
     * @param frames      解码后每包 PCM 帧数（= sampleRate * frameMs / 1000）
     * @param payload     Opus payload 源数组
     * @param payloadLen  payload 实际长度（<= MAX_PAYLOAD）
     * @param flags       MICLINK_FLAG_* 组合
     * @return 包总长度（HEADER_SIZE + payloadLen）
     */
    fun pack(
        dst: ByteArray,
        seq: Int,
        timestampMs: Long,
        sampleRate: Int,
        channels: Int,
        frames: Int,
        payload: ByteArray,
        payloadLen: Int,
        flags: Int
    ): Int {
        require(payloadLen in 0..MAX_PAYLOAD) {
            "payloadLen 越界: $payloadLen（上限 $MAX_PAYLOAD）"
        }
        require(dst.size >= HEADER_SIZE + payloadLen) {
            "目标缓冲过小: ${dst.size} < ${HEADER_SIZE + payloadLen}"
        }

        dst[0] = (MAGIC ushr 24).toByte()
        dst[1] = (MAGIC ushr 16).toByte()
        dst[2] = (MAGIC ushr 8).toByte()
        dst[3] = MAGIC.toByte()

        putBe16(dst, 4, VERSION)
        putBe16(dst, 6, HEADER_SIZE)
        putBe32(dst, 8, seq)
        putBe32(dst, 12, (timestampMs and 0xFFFF_FFFFL).toInt())
        putBe32(dst, 16, sampleRate)
        putBe16(dst, 20, channels)
        putBe16(dst, 22, frames)
        putBe16(dst, 24, payloadLen)
        putBe16(dst, 26, flags)
        putBe32(dst, 28, 0)

        if (payloadLen > 0) {
            System.arraycopy(payload, 0, dst, HEADER_SIZE, payloadLen)
        }
        return HEADER_SIZE + payloadLen
    }

    /**
     * 便捷方法：一步生成独立包体（每帧新建一次 ByteArray，采集线程上会产生 GC 压力，
     * 主采集路径请用 [pack] + 复用缓冲；本方法用于发现/测试/低频路径）。
     */
    fun build(
        seq: Int,
        timestampMs: Long,
        sampleRate: Int,
        channels: Int,
        frames: Int,
        payload: ByteArray,
        payloadLen: Int,
        flags: Int = 0
    ): ByteArray {
        val dst = ByteArray(HEADER_SIZE + payloadLen)
        pack(dst, seq, timestampMs, sampleRate, channels, frames, payload, payloadLen, flags)
        return dst
    }

    /**
     * 自检：产出的头部能否被 shared/core 的 miclink_packet_decode() 接受。
     * 这里用与 protocol.c 完全一致的规则做一次本地校验（不依赖 JNI），
     * 供单元测试 / 首包日志使用。
     *
     * @return 错误信息；null 表示自洽。
     */
    fun selfCheck(packet: ByteArray): String? {
        if (packet.size < HEADER_SIZE) {
            return "包长 ${packet.size} < 头部 $HEADER_SIZE"
        }
        val magic = getBe32(packet, 0)
        if (magic != MAGIC) {
            return "magic 不匹配: 0x${magic.toString(16)}"
        }
        val version = getBe16(packet, 4)
        if (version != VERSION) {
            return "version 不匹配: $version"
        }
        val headerSize = getBe16(packet, 6)
        if (headerSize < HEADER_SIZE || headerSize > 256) {
            return "header_size 非法: $headerSize"
        }
        val payloadBytes = getBe16(packet, 24)
        if (payloadBytes > MAX_PAYLOAD) {
            return "payload_bytes 越界: $payloadBytes"
        }
        val total = headerSize + payloadBytes
        if (packet.size != total) {
            return "长度不自洽: ${packet.size} != $headerSize + $payloadBytes（服务端会拒收）"
        }
        return null
    }

    /** 读回头部字段（调试用）。 */
    fun headerToString(packet: ByteArray): String {
        if (packet.size < HEADER_SIZE) {
            return "<包长不足 ${packet.size}>"
        }
        return "seq=${getBe32(packet, 8)} ts=${getBe32(packet, 12)} rate=${getBe32(packet, 16)} " +
            "ch=${getBe16(packet, 20)} frames=${getBe16(packet, 22)} " +
            "payload=${getBe16(packet, 24)} flags=0x${getBe16(packet, 26).toString(16)}"
    }

    /** 大端写 uint16。 */
    private fun putBe16(dst: ByteArray, off: Int, v: Int) {
        dst[off] = (v ushr 8).toByte()
        dst[off + 1] = v.toByte()
    }

    /** 大端写 uint32。 */
    private fun putBe32(dst: ByteArray, off: Int, v: Int) {
        dst[off] = (v ushr 24).toByte()
        dst[off + 1] = (v ushr 16).toByte()
        dst[off + 2] = (v ushr 8).toByte()
        dst[off + 3] = v.toByte()
    }

    /** 大端读 uint16（无符号）。 */
    fun getBe16(src: ByteArray, off: Int): Int {
        return ((src[off].toInt() and 0xFF) shl 8) or (src[off + 1].toInt() and 0xFF)
    }

    /** 大端读 uint32（按无符号解释，返回 Int 位型）。 */
    fun getBe32(src: ByteArray, off: Int): Int {
        return ((src[off].toInt() and 0xFF) shl 24) or
            ((src[off + 1].toInt() and 0xFF) shl 16) or
            ((src[off + 2].toInt() and 0xFF) shl 8) or
            (src[off + 3].toInt() and 0xFF)
    }

    /**
     * 校验服务端发现应答的文本格式（Discovery 用）。
     * 这里只是占位性工具：把 ByteBuffer 设成大端，保证与 C 端一致的一处公共入口。
     */
    fun bigEndianBuffer(size: Int): ByteBuffer {
        return ByteBuffer.allocate(size).order(java.nio.ByteOrder.BIG_ENDIAN)
    }
}
