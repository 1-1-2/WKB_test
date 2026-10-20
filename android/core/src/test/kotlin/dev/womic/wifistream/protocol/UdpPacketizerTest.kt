// UDP packetizer tests - spec v1 section 3.

package dev.womic.wifistream.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class UdpPacketizerTest {

    private fun bytes(vararg v: Int): ByteArray = ByteArray(v.size) { v[it].toByte() }

    private val opus = bytes(0xDE.toInt(), 0xAD, 0xBE, 0xEF) // stand-in Opus packet

    @Test
    fun `format C header matches spec layout`() {
        val f = AudioFrame(seq = 0x0102, timestampMs = 0x03_04_05_06L, payload = opus, volume = 5)
        val p = UdpPacketizer.packetize(UdpFormat.C, f)
        // 04 00 | len=4+7=11 | seq=0102 | ts=03040506 | vol=05 | deadbeef
        assertArrayEquals(
            bytes(0x04, 0x00, 0x00, 0x0B, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x05,
                0xDE.toInt(), 0xAD, 0xBE, 0xEF),
            p,
        )
    }

    @Test
    fun `format C length field covers header tail`() {
        val f = AudioFrame(1, 0, opus)
        val p = UdpPacketizer.packetize(UdpFormat.C, f)
        val len = ((p[2].toInt() and 0xFF) shl 8) or (p[3].toInt() and 0xFF)
        assertEquals(opus.size + 7, len)
        assertEquals(11 + opus.size, p.size)
    }

    @Test
    fun `format C timestamp wraps at u32`() {
        val f = AudioFrame(0, 0x1_0000_0002L, opus) // 2^32 + 2
        val p = UdpPacketizer.packetize(UdpFormat.C, f)
        val ts = ((p[6].toLong() and 0xFF) shl 24) or
            ((p[7].toLong() and 0xFF) shl 16) or
            ((p[8].toLong() and 0xFF) shl 8) or
            (p[9].toLong() and 0xFF)
        assertEquals(2L, ts)
    }

    @Test
    fun `format B has xor checksum and zero byte`() {
        val f = AudioFrame(seq = 0x00FF, timestampMs = 0, payload = bytes(1, 2, 3))
        val p = UdpPacketizer.packetize(UdpFormat.B, f)
        assertEquals(1 xor 2 xor 3, p[0].toInt() and 0xFF)
        assertEquals(0, p[1].toInt())
        val seq = ((p[2].toInt() and 0xFF) shl 8) or (p[3].toInt() and 0xFF)
        assertEquals(0x00FF, seq)
        val len = ((p[4].toInt() and 0xFF) shl 8) or (p[5].toInt() and 0xFF)
        assertEquals(3, len)
    }

    @Test
    fun `format A is seq u32 plus len u16`() {
        val f = AudioFrame(seq = 0xBEEF, timestampMs = 0, payload = bytes(9, 9))
        val p = UdpPacketizer.packetize(UdpFormat.A, f)
        assertArrayEquals(bytes(0x00, 0x00, 0xBE, 0xEF, 0x00, 0x02, 9, 9), p)
    }

    @Test
    fun `format from variant maps 2 3 4`() {
        assertEquals(UdpFormat.A, UdpFormat.fromVariant(2))
        assertEquals(UdpFormat.B, UdpFormat.fromVariant(3))
        assertEquals(UdpFormat.C, UdpFormat.fromVariant(4))
        try {
            UdpFormat.fromVariant(1)
            throw AssertionError("expected ProtocolError")
        } catch (e: ProtocolError) {
            assertEquals(Status.HELLO_BAD_FORMAT, e.status)
        }
    }

    @Test
    fun `seq wraps like u16`() {
        val f = AudioFrame(seq = 0xFFFF, timestampMs = 0, payload = opus)
        val p = UdpPacketizer.packetize(UdpFormat.C, f)
        assertEquals(0xFF, p[4].toInt() and 0xFF)
        assertEquals(0xFF, p[5].toInt() and 0xFF)
    }
}
