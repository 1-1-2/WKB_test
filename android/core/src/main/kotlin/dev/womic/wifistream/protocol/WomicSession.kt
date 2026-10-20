// Phone-side TCP session controller (server role) - pure logic, no sockets.
//
// Mirrors the official app's V1/l.java request loop and V1/n state machine so
// the wire behaviour can be regression-tested on the JVM (spec v1 section 2.5).

package dev.womic.wifistream.protocol

/** Side effects the controller asks the host (service) to perform. */
sealed interface SessionEvent {
    /** UDP media channel should connect to (peer, mediaPort) and start sending. */
    data class StartStreaming(val mediaPort: Int) : SessionEvent

    /** Streaming should stop (audio thread + UDP channel). */
    data object StopStreaming : SessionEvent

    /** Connection lost / fatal (maps to V1/n.a(reason)). */
    data class Disconnected(val reason: Int) : SessionEvent
}

/** Phone-side connection states (V1/n.f1138a). */
enum class SessionState { IDLE, STARTING, LISTENING, CONNECTED, STREAMING }

/** Everything the controller needs from the host environment. */
interface SessionHost {
    /** Current monotonic clock in ms (SystemClock.elapsedRealtime on Android). */
    fun nowMs(): Long
}

/**
 * Drives one accepted TCP connection.
 *
 * Usage: call [onFrame] for each decoded inbound frame; collect the returned
 * reply bytes (may be empty) and write them to the socket; observe [events].
 */
class WomicSession(private val host: SessionHost) {

    var state: SessionState = SessionState.CONNECTED
        private set

    private val events = ArrayList<SessionEvent>()
    private val replies = ArrayList<ByteArray>()

    /** UDP header format negotiated in hello (null until hello seen). */
    var udpFormat: UdpFormat? = null
        private set

    /** Negotiated streaming parameters. */
    var codec: Int = Codec.OPUS
        private set
    var sampleRate: Int = SampleRates.TABLE[SampleRates.DEFAULT_INDEX]
        private set
    var mediaPort: Int = -1
        private set

    /** Set by heartbeats; cleared by [checkHeartbeatWatchdog]. */
    var heartbeatSeen: Boolean = false
        private set

    fun drainEvents(): List<SessionEvent> = events.toList().also { events.clear() }
    fun drainReplies(): List<ByteArray> = replies.toList().also { replies.clear() }

    /** Handle one inbound frame; queue replies and events. */
    fun onFrame(frame: TcpFrame) {
        when (frame.type) {
            Msg.HELLO -> onHello(frame)
            Msg.CONFIG -> onConfig(frame)
            Msg.START -> onStart(frame)
            Msg.STOP -> onStop(frame)
            Msg.HEARTBEAT -> onHeartbeat(frame)
            else -> replies.add(buildError("Unknown req: ${frame.type}"))
        }
    }

    private fun onHello(frame: TcpFrame) {
        val variant = try {
            parseHello(frame.payload)
        } catch (e: IllegalArgumentException) {
            -1
        }
        if (variant < 2) {
            replies.add(buildHelloReply(Status.HELLO_BAD_FORMAT))
            return
        }
        udpFormat = UdpFormat.fromVariant(variant)
        replies.add(buildHelloReply(Status.OK))
    }

    private fun onConfig(frame: TcpFrame) {
        val params = try {
            ConfigParams.parse(frame.payload)
        } catch (e: IllegalArgumentException) {
            replies.add(buildStatusReply(Msg.CONFIG, Status.CONFIG_BAD_CODEC))
            return
        }
        if (params.codec != Codec.OPUS) {
            // Speex is implemented by the official app but the PC client 6.3
            // only ships Opus; we accept Opus only and answer status 12.
            replies.add(buildStatusReply(Msg.CONFIG, Status.CONFIG_BAD_CODEC))
            return
        }
        if (params.rateIndex !in SampleRates.TABLE.indices) {
            replies.add(buildStatusReply(Msg.CONFIG, Status.CONFIG_BAD_RATE))
            return
        }
        codec = params.codec
        sampleRate = params.sampleRate
        mediaPort = params.mediaPort
        replies.add(buildStatusReply(Msg.CONFIG, Status.OK))
    }

    private fun onStart(frame: TcpFrame) {
        if (mediaPort < 0) {
            replies.add(buildError("start before config"))
            return
        }
        state = SessionState.STREAMING
        events.add(SessionEvent.StartStreaming(mediaPort))
        replies.add(buildStatusReply(Msg.START, Status.OK))
    }

    private fun onStop(frame: TcpFrame) {
        state = SessionState.CONNECTED
        events.add(SessionEvent.StopStreaming)
        // Official app quirk: stop is answered with a type-102 frame (see
        // TcpFrameCodec.buildStopReply and spec v1 section 2.2).
        replies.add(buildStopReply())
    }

    private fun onHeartbeat(frame: TcpFrame) {
        heartbeatSeen = true
        // No reply - official app sends nothing (b2/C0173a.d0 is empty).
    }

    /**
     * Watchdog mirroring V1/k.java: called every 5s; if no heartbeat arrived
     * since the last check the connection is torn down (reason 50).
     */
    fun checkHeartbeatWatchdog(nowMs: Long): Boolean {
        if (heartbeatSeen) {
            heartbeatSeen = false
            return true
        }
        state = SessionState.IDLE
        events.add(SessionEvent.Disconnected(50))
        return false
    }
}
