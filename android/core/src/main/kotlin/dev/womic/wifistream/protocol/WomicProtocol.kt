// WO Mic WiFi protocol conformance core (spec v1).
//
// Wire format (spec v1, docs/protocol/womic-wifi-protocol-v1.md):
//   TCP frame: [type:u8][length:u32 BE][payload:length bytes]
//   All messages big-endian.
//
// Roles: phone = TCP server + UDP sender; PC (official client 6.3) = TCP client + UDP receiver.

package dev.womic.wifistream.protocol

/** Message type bytes on the TCP control channel. */
object Msg {
    const val HELLO = 101 // 0x65 - version/format negotiation
    const val CONFIG = 102 // 0x66 - codec/sample-rate/media-port
    const val START = 103 // 0x67 - start streaming
    const val STOP = 104 // 0x68 - stop streaming
    const val HEARTBEAT = 105 // 0x69 - keepalive, no reply
    const val ERROR = 0 // phone -> PC error string frame
}

/** Codec ids carried by the CONFIG message. */
object Codec {
    const val OPUS = 2
    const val SPEEX = 100
}

/** Sample-rate table indexed by the CONFIG rate byte. */
object SampleRates {
    val TABLE = intArrayOf(8000, 16000, 48000)
    const val DEFAULT_INDEX = 2 // 48000 Hz
}

/** Status codes used in replies. */
object Status {
    const val OK = 0
    const val HELLO_BAD_FORMAT = 10 // hello payload[0] < 2
    const val CONFIG_BAD_CODEC = 12
    const val CONFIG_BAD_RATE = 13
}

object Defaults {
    const val CONTROL_PORT = 8125 // app SharedPreferences "controlPort" (evidence V1/f.java)
    const val VOLUME = 5 // unity gain; UDP header byte 0x0A (evidence V1/f.java)
    const val FRAME_MS = 20 // Opus frame size (evidence V1/m.java)
    const val HEARTBEAT_INTERVAL_MS = 1000L // PC sends ~1s (20kdc trace; to be confirmed on device)
    const val HEARTBEAT_TIMEOUT_MS = 5000L // phone watchdog (evidence V1/k.java)
    const val FORMAT_C = 4 // UDP header format used by official client 6.3
}

/**
 * Hello payload replicated byte-exact from the official Windows client 6.3
 * (objdump of WOMicClient.exe @0x4147be): payload[0]=4 selects UDP header
 * format C; payload[1..5] are skipped by the phone app (evidence d2/a.java).
 */
val HELLO_PAYLOAD_OFFICIAL = byteArrayOf(4, 6, 3, 0, 0, 0)

/** Errors thrown/reported by the session controller. */
class ProtocolError(val status: Int, message: String) : Exception(message)
