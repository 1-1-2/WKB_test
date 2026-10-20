package com.miclink.open

import dev.womic.wifistream.protocol.AudioFrame
import dev.womic.wifistream.protocol.Defaults
import dev.womic.wifistream.protocol.UdpFormat
import dev.womic.wifistream.protocol.UdpPacketizer

/**
 * WomicPacketizer -- 把一帧 Opus payload 封装成 WO Mic WiFi 协议的 UDP 媒体包。
 *
 * 本类是 [dev.womic.wifistream.protocol.UdpPacketizer]（:core 纯 JVM 模块）的薄适配层，
 * 序列化逻辑与字节级测试都在 :core，本文件只做 Android 侧的便捷入口与自检。
 *
 * 【唯一事实来源】`docs/protocol/womic-wifi-protocol-v1.md` 第 3 节：
 * 官方 Windows 客户端 6.3 经 hello 协商选择 **UDP 头格式 C**（variant 4，
 * 证据：s1/i.java 默认分支 + WOMicClient.exe 102 消息端口写入段反汇编）：
 *
 * | 偏移 | 长度 | 字段         | 说明                                          |
 * |------|------|--------------|-----------------------------------------------|
 * | 0    | 2    | magic        | 04 00                                         |
 * | 2    | 2    | len          | opusLen + 7（seq2 + ts4 + vol1），大端         |
 * | 4    | 2    | seq          | 每帧 +1，u16 自然回绕                          |
 * | 6    | 4    | timestamp_ms | SystemClock.elapsedRealtime()，大端            |
 * | 10   | 1    | volume       | 5 = 官方默认单位增益                           |
 * | 11   | *    | opus         | Opus payload                                  |
 *
 * 注意：本工程**不再使用**旧 miclink 自研 WOMP 32 字节头（magic 'WOMP'、
 * FEC/DTX flags 等字段已随协议切换全部废弃，见 git 历史）。
 */
object WomicPacketizer {

    /** 格式 C 头部长度（magic2 + len2 + seq2 + ts4 + vol1）。 */
    const val HEADER_SIZE: Int = 11

    /** 官方默认音量（单位增益）。 */
    const val DEFAULT_VOLUME: Int = Defaults.VOLUME

    /** 官方客户端 6.3 协商使用的 UDP 头格式。 */
    val FORMAT: UdpFormat = UdpFormat.C

    /**
     * 封装一帧为完整 UDP 报文（每次新建 ByteArray）。
     *
     * 采集线程 20ms/帧（50 次/秒）的小对象分配对 Android GC 无压力，
     * 若后续实测发现抖动再在 :core 增加 [UdpPacketizer] 的复用缓冲变体。
     *
     * @param seq          包序号，每帧 +1（u16 回绕）
     * @param timestampMs  采集侧单调时间戳（SystemClock.elapsedRealtime()）
     * @param payload      Opus payload
     * @param volume       音量（0-255，默认 5）
     * @return 完整报文（11 字节头 + payload）
     */
    fun build(
        seq: Int,
        timestampMs: Long,
        payload: ByteArray,
        volume: Int = DEFAULT_VOLUME
    ): ByteArray =
        UdpPacketizer.packetize(FORMAT, AudioFrame(seq, timestampMs, payload, volume))

    /**
     * womic_packet_selfcheck：校验一个已封装报文是否符合格式 C 的自洽规则，
     * 与官方客户端 6.3 的解包逻辑（WOMicClient.exe UDP 接收段反汇编）一致：
     *   - 长度 >= 11
     *   - magic == 04 00
     *   - len 字段 == payload 长度 + 7
     *   - 报文总长 == 11 + (len - 7)，即 4 + len（PC 端按 len+4 取整包）
     *
     * @return 错误信息；null 表示自洽。
     */
    fun selfCheck(packet: ByteArray): String? {
        if (packet.size < HEADER_SIZE) {
            return "包长 ${packet.size} < 格式 C 头部 $HEADER_SIZE"
        }
        if (packet[0] != 0x04.toByte() || packet[1] != 0x00.toByte()) {
            return "magic 不匹配: %02X %02X（期望 04 00）".format(packet[0], packet[1])
        }
        val len = ((packet[2].toInt() and 0xFF) shl 8) or (packet[3].toInt() and 0xFF)
        if (len < 7) {
            return "len 字段非法: $len（至少 7）"
        }
        val payloadLen = len - 7
        // PC 端按 len+4 读取整包：头部 4 字节（magic2+len2）+ seq2+ts4+vol1 与 payload
        if (packet.size != len + 4) {
            return "长度不自洽: ${packet.size} != len($len) + 4"
        }
        return null
    }

    /** 读回头部字段（日志/调试用）。 */
    fun headerToString(packet: ByteArray): String {
        if (packet.size < HEADER_SIZE) {
            return "<包长不足 ${packet.size}>"
        }
        val len = ((packet[2].toInt() and 0xFF) shl 8) or (packet[3].toInt() and 0xFF)
        val seq = ((packet[4].toInt() and 0xFF) shl 8) or (packet[5].toInt() and 0xFF)
        val ts = ((packet[6].toLong() and 0xFF) shl 24) or
            ((packet[7].toLong() and 0xFF) shl 16)or
            ((packet[8].toLong() and 0xFF) shl 8)or
            (packet[9].toLong() and 0xFF)
        return "len=$len seq=$seq ts=$ts vol=${packet[10].toInt() and 0xFF}"
    }
}
