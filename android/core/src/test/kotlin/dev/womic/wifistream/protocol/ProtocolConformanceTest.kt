// Protocol conformance tests - spec v1 (docs/protocol/womic-wifi-protocol-v1.md).
//
// Vectors marked "official" are built from the disassembly of the official
// Windows client 6.3 (WOMicClient.exe); vectors marked "trace" come from the
// 20kdc Wireshark capture of the official pair.

package dev.womic.wifistream.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ProtocolConformanceTest {

    private fun bytes(vararg v: Int): ByteArray =
        ByteArray(v.size) { v[it].toByte() }

    private fun ByteArray.hex(): String = joinToString(" ") { "%02X".format(it) }

    // ------------------------------------------------------------ framing

    @Test
    fun `frame encodes type and big-endian length`() {
        val f = FrameDecoder.encode(Msg.START, ByteArray(0))
        assertEquals("67 00 00 00 00", f.hex())
    }

    @Test
    fun `decoder reassembles fragmented frames`() {
        val wire = buildConfig(Codec.OPUS, 2, 0x1234)
        // feed byte-by-byte
        val dec = FrameDecoder()
        val frames = ArrayList<TcpFrame>()
        for (b in wire) frames.addAll(dec.feed(byteArrayOf(b), 1))
        assertEquals(1, frames.size)
        assertEquals(Msg.CONFIG, frames[0].type)
        val p = ConfigParams.parse(frames[0].payload)
        assertEquals(Codec.OPUS, p.codec)
        assertEquals(2, p.rateIndex)
        assertEquals(48000, p.sampleRate)
        assertEquals(0x1234, p.mediaPort)
    }

    @Test
    fun `decoder splits coalesced frames`() {
        val wire = buildHeartbeat() + buildStart()
        val frames = FrameDecoder().feed(wire)
        assertEquals(2, frames.size)
        assertEquals(Msg.HEARTBEAT, frames[0].type)
        assertEquals(Msg.START, frames[1].type)
    }

    // ------------------------------------------------------------ hello

    @Test
    fun `hello from official client 6_3 (official)`() {
        // WOMicClient.exe @0x4147be: 65 00000006 04 06 03 00 00 00
        val f = FrameDecoder().decodeSingle(buildHello())
        assertEquals(Msg.HELLO, f.type)
        assertEquals(6, f.payload.size)
        assertArrayEquals(HELLO_PAYLOAD_OFFICIAL, f.payload)
        assertEquals(4, parseHello(f.payload)) // format C
    }

    @Test
    fun `hello reply matches official app (trace)`() {
        val r = buildHelloReply()
        assertEquals("65 00 00 00 02 00 04", r.hex())
    }

    @Test
    fun `hello reply error 10 for variant below 2`() {
        val s = WomicSession(FakeHost)
        s.onFrame(TcpFrame(Msg.HELLO, bytes(1, 0, 0, 0, 0, 0)))
        val replies = s.drainReplies()
        assertEquals(1, replies.size)
        assertEquals("65 00 00 00 02 0A 04", replies[0].hex()) // 0x0A = 10
        assertEquals(null, s.udpFormat)
    }

    // ------------------------------------------------------------ config

    @Test
    fun `config from official client 6_3 (official)`() {
        // WOMicClient.exe @0x414a56: 66 00000006 02 02 <port BE>
        val f = FrameDecoder().decodeSingle(buildConfig(Codec.OPUS, 2, 4660))
        assertEquals(Msg.CONFIG, f.type)
        assertEquals("66 00 00 00 06 02 02 00 00 12 34", FrameDecoder.encode(f.type, f.payload).hex())
    }

    @Test
    fun `config reply ok (trace)`() {
        assertEquals("66 00 00 00 01 00", buildStatusReply(Msg.CONFIG, Status.OK).hex())
    }

    @Test
    fun `config rejects speex with status 12`() {
        val s = WomicSession(FakeHost)
        s.onFrame(TcpFrame(Msg.HELLO, HELLO_PAYLOAD_OFFICIAL))
        s.drainReplies()
        s.onFrame(TcpFrame(Msg.CONFIG, bytes(100, 0, 0, 0, 0x1F, 0x90)))
        val r = s.drainReplies()
        assertEquals(1, r.size)
        assertEquals("66 00 00 00 01 0C", r[0].hex()) // 0x0C = 12
    }

    @Test
    fun `config rejects bad rate index with status 13`() {
        val s = WomicSession(FakeHost)
        s.onFrame(TcpFrame(Msg.HELLO, HELLO_PAYLOAD_OFFICIAL))
        s.drainReplies()
        s.onFrame(TcpFrame(Msg.CONFIG, bytes(2, 3, 0, 0, 0x1F, 0x90)))
        val r = s.drainReplies()
        assertEquals("66 00 00 00 01 0D", r[0].hex()) // 0x0D = 13
    }

    // ------------------------------------------------------------ start/stop

    @Test
    fun `start reply (trace)`() {
        assertEquals("67 00 00 00 01 00", buildStatusReply(Msg.START, Status.OK).hex())
    }

    @Test
    fun `stop reply is a type-102 frame (official quirk)`() {
        val s = WomicSession(FakeHost)
        s.onFrame(TcpFrame(Msg.HELLO, HELLO_PAYLOAD_OFFICIAL))
        s.onFrame(TcpFrame(Msg.CONFIG, bytes(2, 2, 0, 0, 0x1F, 0x90)))
        s.drainReplies()
        s.onFrame(TcpFrame(Msg.START, ByteArray(0)))
        s.drainReplies()
        val ev = s.drainEvents()
        assertTrue(ev.contains(SessionEvent.StartStreaming(0x1F90)))
        assertEquals(SessionState.STREAMING, s.state)

        s.onFrame(TcpFrame(Msg.STOP, ByteArray(0)))
        val r = s.drainReplies()
        assertEquals(1, r.size)
        assertEquals("66 00 00 00 01 00", r[0].hex()) // type 102, not 104!
        assertTrue(s.drainEvents().contains(SessionEvent.StopStreaming))
        assertEquals(SessionState.CONNECTED, s.state)
    }

    // ------------------------------------------------------------ heartbeat

    @Test
    fun `heartbeat gets no reply (official)`() {
        val s = WomicSession(FakeHost)
        s.onFrame(TcpFrame(Msg.HEARTBEAT, ByteArray(0)))
        assertEquals(0, s.drainReplies().size)
        assertTrue(s.heartbeatSeen)
    }

    @Test
    fun `watchdog tears down after 5s without heartbeat`() {
        val s = WomicSession(FakeHost)
        // Official V1/k.java: the beat flag starts false, so the very first
        // 5s tick without a heartbeat drops the link (reason 50).
        assertFalse(s.checkHeartbeatWatchdog(0))
        val ev = s.drainEvents()
        assertTrue(ev.contains(SessionEvent.Disconnected(50)))
        assertEquals(SessionState.IDLE, s.state)
    }

    @Test
    fun `watchdog survives while heartbeats keep arriving`() {
        val s = WomicSession(FakeHost)
        s.onFrame(TcpFrame(Msg.HEARTBEAT, ByteArray(0)))
        assertTrue(s.checkHeartbeatWatchdog(0))  // beat seen -> flag cleared, survive
        assertFalse(s.checkHeartbeatWatchdog(0)) // no new beat -> drop
    }

    @Test
    fun `unknown message type gets error frame`() {
        val s = WomicSession(FakeHost)
        s.onFrame(TcpFrame(7, ByteArray(0)))
        val r = s.drainReplies()
        assertEquals(1, r.size)
        assertEquals(0, r[0][0].toInt()) // type 0
        assertEquals("Unknown req: 7", String(r[0].copyOfRange(6, r[0].size), Charsets.US_ASCII))
    }

    private object FakeHost : SessionHost {
        override fun nowMs(): Long = 0
    }
}

/** Small helper: decode exactly one frame. */
fun FrameDecoder.decodeSingle(wire: ByteArray): TcpFrame {
    val frames = feed(wire)
    require(frames.size == 1) { "expected 1 frame, got ${frames.size}" }
    return frames[0]
}
