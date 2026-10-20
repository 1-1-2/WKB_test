// UDP media packetizer - WO Mic WiFi (spec v1 section 3).
//
// Three header formats exist; the official client 6.3 uses format C
// (variant 4, evidence s1/i.java default branch + WOMicClient.exe 102 msg):
//
//   offset 00: 04 00            magic
//   offset 02: len  u16 BE      = opusLen + 7 (seq2 + ts4 + vol1)
//   offset 04: seq  u16 BE      per-frame counter, wraps
//   offset 06: ts   u32 BE      SystemClock.elapsedRealtime() milliseconds
//   offset 0A: vol  u8          5 = unity gain (official default)
//   offset 0B: opus payload

package dev.womic.wifistream.protocol

/** UDP header format variants selected by the hello payload[0]. */
enum class UdpFormat(val variant: Int) {
    /** [seq:u32][len:u16][opus] - no magic. */
    A(2),

    /** [xor:u8][00][seq:u16][len:u16][opus]. */
    B(3),

    /** [04 00][len:u16][seq:u16][ts:u32][vol:u8][opus] - official client 6.3. */
    C(4);

    companion object {
        fun fromVariant(v: Int): UdpFormat =
            entries.firstOrNull { it.variant == v }
                ?: throw ProtocolError(Status.HELLO_BAD_FORMAT, "unsupported format variant $v")
    }
}

/** One captured/encoded audio frame ready for the wire. */
class AudioFrame(
    val seq: Int,
    val timestampMs: Long,
    val payload: ByteArray,
    val volume: Int = Defaults.VOLUME,
) {
    init {
        require(seq in 0..0xFFFF) { "seq must fit u16" }
        require(volume in 0..255) { "volume must fit u8" }
    }
}

/** Serializes [AudioFrame] into a UDP datagram for the chosen header format. */
object UdpPacketizer {

    fun packetize(format: UdpFormat, frame: AudioFrame): ByteArray = when (format) {
        UdpFormat.A -> formatA(frame)
        UdpFormat.B -> formatB(frame)
        UdpFormat.C -> formatC(frame)
    }

    /** [seq:u32 BE][len:u16 BE][opus] */
    private fun formatA(f: AudioFrame): ByteArray {
        val out = ByteArray(6 + f.payload.size)
        writeU32BE(out, 0, f.seq.toLong() and 0xFFFFFFFFL)
        writeU16BE(out, 4, f.payload.size)
        f.payload.copyInto(out, 6)
        return out
    }

    /** [xor:u8][00][seq:u16][len:u16][opus] - xor over opus bytes. */
    private fun formatB(f: AudioFrame): ByteArray {
        val xor = f.payload.fold(0) { acc, b -> acc xor (b.toInt() and 0xFF) }
        val out = ByteArray(6 + f.payload.size)
        out[0] = xor.toByte()
        out[1] = 0
        writeU16BE(out, 2, f.seq)
        writeU16BE(out, 4, f.payload.size)
        f.payload.copyInto(out, 6)
        return out
    }

    /** [04 00][len+7:u16][seq:u16][ts:u32][vol:u8][opus] */
    private fun formatC(f: AudioFrame): ByteArray {
        val out = ByteArray(11 + f.payload.size)
        out[0] = 0x04
        out[1] = 0x00
        writeU16BE(out, 2, f.payload.size + 7)
        writeU16BE(out, 4, f.seq)
        writeU32BE(out, 6, f.timestampMs and 0xFFFFFFFFL)
        out[10] = f.volume.toByte()
        f.payload.copyInto(out, 11)
        return out
    }

    internal fun writeU16BE(b: ByteArray, off: Int, v: Int) {
        b[off] = ((v ushr 8) and 0xFF).toByte()
        b[off + 1] = (v and 0xFF).toByte()
    }

    internal fun writeU32BE(b: ByteArray, off: Int, v: Long) {
        b[off] = ((v ushr 24) and 0xFF).toByte()
        b[off + 1] = ((v ushr 16) and 0xFF).toByte()
        b[off + 2] = ((v ushr 8) and 0xFF).toByte()
        b[off + 3] = (v and 0xFF).toByte()
    }
}
