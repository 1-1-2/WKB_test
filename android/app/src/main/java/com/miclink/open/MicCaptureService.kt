package com.miclink.open

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import dev.womic.wifistream.protocol.Codec
import dev.womic.wifistream.protocol.Defaults
import dev.womic.wifistream.protocol.FrameDecoder
import dev.womic.wifistream.protocol.SampleRates
import dev.womic.wifistream.protocol.SessionEvent
import dev.womic.wifistream.protocol.SessionHost
import dev.womic.wifistream.protocol.WomicSession
import java.io.IOException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.util.Collections
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.PI
import kotlin.math.sin

/**
 * MicCaptureService -- WO Mic 手机侧前台服务：TCP 控制服务器 + UDP 媒体发送。
 *
 * 链路（角色与官方 app 完全一致，规格 v1 第 1 节）：
 * ```
 * PC 客户端 (WOMicClient.exe)
 *   |                                     ^
 *   |-- TCP 8125: hello/config/start/stop/heartbeat --> 本服务（WomicSession）
 *   |                                     |
 *   +-- UDP <mediaPort> <---- Opus 格式 C 包 <------+
 *         AudioRecord -> OpusEncoder -> WomicPacketizer -> UdpSender
 * ```
 *
 * 关键设计：
 *   1) **手机是 TCP server**：启动后监听控制端口（默认 8125），等待 PC 客户端连入；
 *      连接由 [serverLoop] accept，[connectionLoop] 驱动 [WomicSession] 状态机，
 *      PC 发 START 才真正开始采集推流（对应 [SessionEvent.StartStreaming]）；
 *   2) **心跳看门狗**：镜像官方 V1/k.java —— 每 5s 检查一次，期间无 105 心跳即断开；
 *   3) **前台服务 + PARTIAL_WAKE_LOCK**：息屏/切后台后仍持续推流；
 *   4) **硬件热切换自愈**：AudioDeviceCallback 置位后重建 AudioRecord；
 *   5) **调试音模式**：无麦克风权限（或只想验证链路）时发送 440Hz 正弦，
 *      不创建 AudioRecord；
 *   6) **采集选项**（音源模式 / 麦克风 / 系统 NS/AEC/AGC）：见 [CaptureSettings]，
 *      默认值 = 改动前行为；调试音模式下这些选项一律不生效（没有 AudioRecord）；
 *   7) **协议层全部委托 :core**（帧编解码/会话/包化均有字节级单元测试），
 *      本类只负责 Android 侧的线程与资源编排。
 */
class MicCaptureService : Service() {

    /** UI 侧状态回调（全部在主线程执行）。 */
    interface CaptureStatusListener {
        /** 状态变化：state 见 [STATE_IDLE] / [STATE_LISTENING] / [STATE_CONNECTED] / [STATE_STREAMING] 等。 */
        fun onState(state: String, detail: String)

        /** 电平更新（约 20 次/秒）。 */
        fun onLevel(rmsDb: Float, peakDb: Float)

        /** 出错（服务会自行停止）。 */
        fun onError(message: String)

        /**
         * 非致命提示（服务继续运行）：如"无法切换到所选麦克风，已回退系统默认设备"。
         * 与 [onError] 的区别：本通道不停止服务，UI 弹出一次性对话框即可。
         */
        fun onMessage(message: String)
    }

    companion object {
        private const val TAG = "MicCaptureService"

        const val ACTION_START = "com.miclink.open.action.START"
        const val ACTION_STOP = "com.miclink.open.action.STOP"

        const val EXTRA_CONTROL_PORT = "extra.control_port"
        const val EXTRA_VOLUME = "extra.volume"
        const val EXTRA_DEBUG_TONE = "extra.debug_tone"

        // 采集选项（CaptureSettings，均有默认值：缺省即改动前行为）
        const val EXTRA_AUDIO_SOURCE = "extra.audio_source"
        const val EXTRA_MIC_DEVICE_ID = "extra.mic_device_id"
        const val EXTRA_NOISE_SUPPRESSOR = "extra.noise_suppressor"
        const val EXTRA_ECHO_CANCELER = "extra.echo_canceler"
        const val EXTRA_GAIN_CONTROL = "extra.gain_control"

        const val CHANNEL_ID = "womic_capture"
        const val CHANNEL_NAME = "WO Mic 麦克风采集"
        const val NOTIFICATION_ID = 0x574F4D50.toInt()

        const val STATE_IDLE = "空闲"
        const val STATE_STARTING = "启动中"
        const val STATE_LISTENING = "等待连接"
        const val STATE_CONNECTED = "已连接"
        const val STATE_STREAMING = "推流中"
        const val STATE_STOPPED = "已停止"
        const val STATE_ERROR = "错误"

        /** 连续发送失败达到该值即认为网络不可用，停止采集。 */
        private const val MAX_SEND_FAILURES = 200

        /** AudioRecord 重建的最大连续次数（超过则报错停止）。 */
        private const val MAX_RESTARTS = 5

        /** 重建之间的退避（ms）。 */
        private const val RESTART_BACKOFF_MS = 300L

        /** 电平回调节流（ms）：约 20 次/秒。 */
        private const val LEVEL_INTERVAL_MS = 50L

        /** 通知文案刷新节流（ms）。 */
        private const val NOTIFY_INTERVAL_MS = 2000L

        /** 心跳看门狗周期：与 BuildConfig.HEARTBEAT_TIMEOUT_MS（官方 V1/k.java 5s）一致。 */
        private val WATCHDOG_PERIOD_MS: Long = BuildConfig.HEARTBEAT_TIMEOUT_MS

        /** 调试音频率（Hz）与幅度（满量程比例）。 */
        private const val TONE_FREQUENCY_HZ = 440.0
        private const val TONE_AMPLITUDE = 0.3

        /** Opus 编码复杂度（0..10）：官方 app 用默认 5。 */
        private const val OPUS_COMPLEXITY = 5

        /** 服务是否已启动（server socket 已打开），供 UI 同步按钮状态。 */
        @Volatile
        var isRunning: Boolean = false

        /** 是否正在推流（PC 已发 START 且采集线程在跑）。 */
        @Volatile
        var isStreaming: Boolean = false
            private set

        /** 最后一次错误原因（UI 显示用）。 */
        @Volatile
        var lastError: String? = null

        /** 最近一次采集的实际采样率（平台可能调整请求值，见 [MicCaptureService.openRecorder]）。 */
        @Volatile
        var actualSampleRate: Int = 0

        /**
         * 构造启动 Intent（建议用本方法，避免漏传参数）。
         */
        fun buildStartIntent(context: Context, config: Prefs.Config): Intent {
            return Intent(context, MicCaptureService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_CONTROL_PORT, config.controlPort)
                putExtra(EXTRA_VOLUME, config.volume)
                putExtra(EXTRA_DEBUG_TONE, config.debugTone)
                putExtra(EXTRA_AUDIO_SOURCE, config.capture.audioSource)
                putExtra(EXTRA_MIC_DEVICE_ID, config.capture.micDeviceId)
                putExtra(EXTRA_NOISE_SUPPRESSOR, config.capture.noiseSuppressor)
                putExtra(EXTRA_ECHO_CANCELER, config.capture.echoCanceler)
                putExtra(EXTRA_GAIN_CONTROL, config.capture.gainControl)
            }
        }

        fun buildStopIntent(context: Context): Intent {
            return Intent(context, MicCaptureService::class.java).apply {
                action = ACTION_STOP
            }
        }

        /** 协议编码器 id -> 可读名。 */
        fun codecName(codec: Int): String = when (codec) {
            Codec.OPUS -> "Opus"
            Codec.SPEEX -> "Speex"
            else -> "codec#$codec"
        }
    }

    // ------------------------------------------------------------------
    // 字段
    // ------------------------------------------------------------------
    private val binder = LocalBinder()
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    private var listener: CaptureStatusListener? = null

    // TCP server（服务级）
    @Volatile
    private var serverThread: Thread? = null
    @Volatile
    private var serverSocket: ServerSocket? = null

    // 单连接（连接级）
    @Volatile
    private var connThread: Thread? = null
    @Volatile
    private var watchdogThread: Thread? = null
    @Volatile
    private var connection: Socket? = null

    /** 当前连接的会话状态机（:core 纯逻辑，无 socket）。 */
    @Volatile
    private var session: WomicSession? = null

    // 采集线程与媒体链路（推流级）
    @Volatile
    private var captureThread: Thread? = null
    @Volatile
    private var recorder: AudioRecord? = null

    /** 系统效果（NS/AEC/AGC）：跟 AudioRecord 会话走，重建录音时先 release 再 attach。 */
    @Volatile
    private var audioEffects: SystemAudioEffects? = null

    /** 输入设备枚举 / 能力查询（framework 薄封装，懒加载避免 onCreate 之外的无谓初始化）。 */
    private val deviceManager: AudioDeviceManager by lazy { AudioDeviceManager(this) }

    // encoder / udpSender 由主线程创建释放、采集线程读写，必须 volatile。
    @Volatile
    private var encoder: OpusEncoder? = null
    @Volatile
    private var udpSender: UdpSender? = null

    private var wakeLock: PowerManager.WakeLock? = null
    private var audioManager: AudioManager? = null

    /** 硬件（麦克风/耳机/USB 声卡）发生变化，需要重建 AudioRecord。 */
    private val hardwareChanged = AtomicBoolean(false)

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
            hardwareChanged.set(true)
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
            hardwareChanged.set(true)
        }
    }

    // 运行期参数（主线程写、采集线程读）
    @Volatile
    private var controlPort: Int = Prefs.DEFAULT_CONTROL_PORT
    @Volatile
    private var volume: Int = Prefs.DEFAULT_VOLUME
    @Volatile
    private var debugTone: Boolean = false

    /**
     * 采集选项（主线程经 [updateCaptureSettings] 写、采集线程经 [openRecorder] 读）。
     * 默认值 = 改动前行为：DEFAULT 音源 / 跟随系统设备 / 无系统效果。
     */
    @Volatile
    private var captureSettings: CaptureSettings = CaptureSettings()

    /** 采集选项变更（UI 推送）后需要重建 AudioRecord（调试音模式下忽略）。 */
    private val captureSettingsChanged = AtomicBoolean(false)

    /** 协商出的编码参数（CONFIG 消息，主线程写、UI 线程读）。 */
    @Volatile
    private var negotiatedSampleRate: Int = 0
    @Volatile
    private var negotiatedMediaPort: Int = -1
    @Volatile
    private var peerDescription: String = ""

    /** 每帧样本数（由协商采样率 + 20ms 推导）。 */
    @Volatile
    private var framesPerPacket: Int = 0

    // ------------------------------------------------------------------
    // 生命周期
    // ------------------------------------------------------------------
    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        audioManager = getSystemService(Context.AUDIO_SERVICE) as? AudioManager
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopCapture(STATE_STOPPED, "用户停止")
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_START -> {
                controlPort = intent.getIntExtra(EXTRA_CONTROL_PORT, Prefs.DEFAULT_CONTROL_PORT)
                volume = intent.getIntExtra(EXTRA_VOLUME, Prefs.DEFAULT_VOLUME)
                debugTone = intent.getBooleanExtra(EXTRA_DEBUG_TONE, false)
                captureSettings = readCaptureSettings(intent)
                startCapture()
            }
            else -> {
                // 系统重启服务（START_STICKY 场景）时没有参数：直接停止，避免空跑。
                stopSelf()
            }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        stopCapture(STATE_STOPPED, "服务销毁")
        super.onDestroy()
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        // stopWithTask=true：任务被划掉时系统会杀掉进程，这里做一次显式清理。
        stopCapture(STATE_STOPPED, "任务已移除")
        super.onTaskRemoved(rootIntent)
    }

    inner class LocalBinder : Binder() {
        fun getService(): MicCaptureService = this@MicCaptureService
    }

    // ------------------------------------------------------------------
    // 对外 API（供 Activity 使用）
    // ------------------------------------------------------------------

    /** 注册状态回调（主线程调用）。 */
    fun setStatusListener(l: CaptureStatusListener?) {
        listener = l
        // 注册时立刻回灌一次当前状态，避免 UI 与后台状态不一致
        l?.onState(currentState(), currentDetail())
    }

    /** 当前状态的中文描述。 */
    fun currentState(): String = when {
        isStreaming -> STATE_STREAMING
        isConnected() -> STATE_CONNECTED
        isRunning -> STATE_LISTENING
        lastError != null -> STATE_ERROR
        else -> STATE_IDLE
    }

    /** 当前状态细节（监听地址 / 对端 / 协商参数 + 统计）。 */
    fun currentDetail(): String {
        val sender = udpSender
        return when {
            isStreaming && sender != null ->
                "→ ${peerDescription} · ${sender.targetDescription()} · " +
                    "${sender.packetsSent} 包 · ${actualSampleRate}Hz"
            isConnected() ->
                "PC 已连接：${peerDescription} · 协商 ${codecName(negotiatedCodec())}/" +
                    "${negotiatedSampleRate}Hz/媒体端口 ${negotiatedMediaPort}"
            isRunning ->
                "等待 PC 连接 · 手机 ${listeningAddress()}"
            else ->
                lastError ?: "未启动"
        }
    }

    /** 本机监听地址（IPv4:port），UI 提示用户在 PC 客户端填写。 */
    fun listeningAddress(): String {
        val port = if (controlPort in 1..65535) controlPort else Prefs.DEFAULT_CONTROL_PORT
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            for (ni in Collections.list(interfaces)) {
                if (!ni.isUp || ni.isLoopback) continue
                for (addr in Collections.list(ni.inetAddresses)) {
                    if (!addr.isLoopbackAddress && addr is Inet4Address && addr.isSiteLocalAddress) {
                        return "${addr.hostAddress}:$port"
                    }
                }
            }
        } catch (e: SocketException) {
            android.util.Log.w(TAG, "枚举网卡失败：${e.message}")
        }
        return "0.0.0.0:$port"
    }

    /** 当前协商的编码器 id（未连接时为 Opus 默认）。 */
    fun negotiatedCodec(): Int = session?.codec ?: Codec.OPUS

    /** 已发送包数（UI 轮询显示）。 */
    fun packetsSent(): Long = udpSender?.packetsSent ?: 0L

    /** 已发送字节数。 */
    fun bytesSent(): Long = udpSender?.bytesSent ?: 0L

    /** 连续发送失败次数。 */
    fun sendFailures(): Int = udpSender?.failureStreak ?: 0

    /**
     * 当前实际生效的输入设备名（[AudioRecord.getRoutedDevice]，API 24+）。
     *
     * 与"用户选了啥"是两回事：设备可能失效/被系统改路由，UI 必须显示这个值
     * 而不是偏好值。未在录音（无 AudioRecord）时返回 null。
     */
    fun routedDeviceLabel(): String? {
        val rec = recorder ?: return null
        return try {
            rec.routedDevice?.let { AudioDeviceManager.deviceLabel(it) }
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "读取 routedDevice 失败：${t.message}")
            null
        }
    }

    /**
     * 更新采集选项（UI 在主线程调用，见 [MainViewModel.pushCaptureSettings]）。
     *
     * **变更即时生效**：正在推流时置位 [captureSettingsChanged]，采集线程走与
     * "硬件热切换"同一条重建路径释放并重建 AudioRecord（含重新应用设备偏好与
     * 系统效果），用户无需停止推流；未推流时只记录，下次 START 经 Intent 带入。
     * 选择"即时生效"而非"下次录音时生效"的理由：麦克风/音源切换的典型场景就是
     * 会话进行中（插入 USB 声卡、发现默认音源被系统改坏），要求用户先断链重来
     * 体验更差；且重建路径本就为硬件热插拔存在，已被现有代码验证过。
     */
    fun updateCaptureSettings(settings: CaptureSettings) {
        val sanitized = settings.sanitize(unprocessedSupported = deviceManager.isUnprocessedSupported())
        if (sanitized == captureSettings) {
            return
        }
        captureSettings = sanitized
        captureSettingsChanged.set(true)
        android.util.Log.i(
            TAG,
            "采集选项已更新：audioSource=${sanitized.audioSource}，" +
                "micDeviceId=${sanitized.micDeviceId}，" +
                "ns=${sanitized.noiseSuppressor} aec=${sanitized.echoCanceler} " +
                "agc=${sanitized.gainControl}"
        )
        if (isStreaming) {
            postMessage(getString(R.string.capture_msg_settings_applied))
        }
    }

    /** 从 START Intent 读取采集选项并清洗（未知音源/不支持的 UNPROCESSED 回落 DEFAULT）。 */
    private fun readCaptureSettings(intent: Intent): CaptureSettings {
        val raw = CaptureSettings(
            audioSource = intent.getIntExtra(EXTRA_AUDIO_SOURCE, AudioSourceIds.DEFAULT),
            micDeviceId = intent.getIntExtra(EXTRA_MIC_DEVICE_ID, CaptureSettings.MIC_DEVICE_AUTO),
            noiseSuppressor = intent.getBooleanExtra(EXTRA_NOISE_SUPPRESSOR, false),
            echoCanceler = intent.getBooleanExtra(EXTRA_ECHO_CANCELER, false),
            gainControl = intent.getBooleanExtra(EXTRA_GAIN_CONTROL, false)
        )
        return raw.sanitize(unprocessedSupported = deviceManager.isUnprocessedSupported())
    }

    private fun isConnected(): Boolean = connection?.isClosed == false && connection != null

    // ------------------------------------------------------------------
    // 服务启停
    // ------------------------------------------------------------------

    /** 启动服务：上前台通知，打开 TCP 控制端口，进入监听。 */
    private fun startCapture() {
        if (isRunning) {
            return
        }
        lastError = null
        actualSampleRate = 0
        notifyState(STATE_STARTING, "正在启动…")

        // 1) 前台通知：必须在 startForegroundService 之后尽快调用，否则会被系统判为 ANR/崩溃。
        //    Android 14+ 对 microphone 型前台服务有权限校验；拿不到麦克风权限时逐级降级，
        //    保证"调试音模式"在无麦克风权限的设备上仍可用。
        if (!startForegroundSafely()) {
            fail("无法启动前台服务（请确认已授予通知权限）")
            return
        }

        // 2) 参数校验
        if (controlPort !in 1..65535) {
            fail("控制端口非法：$controlPort")
            return
        }
        if (volume !in 0..255) {
            fail("音量非法：$volume（协议字段为 u8）")
            return
        }

        // 3) WakeLock：息屏后保持 CPU 运行
        acquireWakeLock()

        // 4) TCP 控制服务器（手机 = server，等 PC 客户端连入）
        val sock = try {
            ServerSocket(controlPort)
        } catch (e: IOException) {
            fail("无法监听控制端口 $controlPort：${e.message}（可能被其他应用占用）")
            releaseWakeLock()
            return
        }
        serverSocket = sock

        // 5) accept 循环
        isRunning = true
        val thread = Thread({ serverLoop(sock) }, "womic-tcp-server")
        serverThread = thread
        thread.start()

        notifyState(STATE_LISTENING, "等待 PC 连接 · 手机 ${listeningAddress()}")
        updateNotification("等待 PC 连接 · ${listeningAddress()}")
    }

    /** 停止服务并释放全部资源（可重复调用）。 */
    private fun stopCapture(state: String, reason: String) {
        // 采集链路
        stopStreaming("服务停止")

        // 连接与服务器
        teardownConnection("服务停止", notify = false)
        val sock = serverSocket
        serverSocket = null
        serverThread?.interrupt()
        serverThread = null
        if (sock != null) {
            try {
                sock.close()
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "关闭 ServerSocket 异常：${t.message}")
            }
        }

        try {
            audioManager?.unregisterAudioDeviceCallback(deviceCallback)
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "注销 AudioDeviceCallback 失败：${t.message}")
        }

        releaseWakeLock()
        isRunning = false

        try {
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "stopForeground 失败：${t.message}")
        }

        if (state == STATE_ERROR) {
            lastError = reason
        } else {
            lastError = null
        }
        notifyState(state, reason)
    }

    /** 出错停止：把原因回传给 UI。 */
    private fun fail(message: String) {
        android.util.Log.e(TAG, message)
        stopCapture(STATE_ERROR, message)
        mainHandler.post { listener?.onError(message) }
        try {
            stopSelf()
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "stopSelf 失败：${t.message}")
        }
    }

    // ------------------------------------------------------------------
    // TCP 服务器与连接驱动
    // ------------------------------------------------------------------

    /** accept 循环：一次只服务一个客户端，断开后继续等下一个。 */
    private fun serverLoop(sock: ServerSocket) {
        android.util.Log.i(TAG, "TCP 控制服务器已监听 :$controlPort")
        while (!Thread.currentThread().isInterrupted && !sock.isClosed) {
            try {
                val client = sock.accept()
                synchronized(this) {
                    // 已有连接：礼貌拒绝（官方 app 同样只服务一个客户端）
                    val existing = connection
                    if (existing != null && !existing.isClosed) {
                        try {
                            client.close()
                        } catch (_: IOException) {
                        }
                        continue
                    }
                    connection = client
                    peerDescription = client.inetAddress.hostAddress ?: "?"
                    val thread = Thread({ connectionLoop(client) }, "womic-conn")
                    connThread = thread
                    thread.start()
                }
                android.util.Log.i(TAG, "PC 客户端已连接：${peerDescription}")
                notifyState(STATE_CONNECTED, "PC 已连接：${peerDescription}")
                updateNotification("已连接 ${peerDescription}，等待协商…")
            } catch (e: IOException) {
                if (!sock.isClosed) {
                    android.util.Log.w(TAG, "accept 失败：${e.message}")
                }
                break
            }
        }
        android.util.Log.i(TAG, "TCP 控制服务器退出")
    }

    /**
     * 单连接驱动：阻塞读 -> FrameDecoder -> [WomicSession] -> 写回应答 -> 分派事件。
     * 返回即连接结束（对端关闭/异常/看门狗超时）。
     */
    private fun connectionLoop(client: Socket) {
        var closeReason = "连接结束"
        try {
            client.tcpNoDelay = true
            val input = client.getInputStream()
            val output = client.getOutputStream()
            val decoder = FrameDecoder()
            val sess = WomicSession(object : SessionHost {
                override fun nowMs(): Long = SystemClock.elapsedRealtime()
            })
            session = sess

            // 心跳看门狗：镜像官方 V1/k.java（每 5s，无心跳即断）
            startWatchdog(sess, client)

            val buf = ByteArray(4096)
            while (!Thread.currentThread().isInterrupted && !client.isClosed) {
                val n = input.read(buf)
                if (n < 0) {
                    closeReason = "PC 客户端关闭了连接"
                    break
                }
                val frames = decoder.feed(buf, n)
                for (frame in frames) {
                    sess.onFrame(frame)
                    // 先写应答（协议顺序：PC 收到 status 才认为请求完成）
                    val replies = sess.drainReplies()
                    for (r in replies) {
                        output.write(r)
                    }
                    if (replies.isNotEmpty()) {
                        output.flush()
                    }
                    // 再分派副作用事件（start/stop/disconnect）
                    val events = sess.drainEvents()
                    for (event in events) {
                        dispatchEvent(event, peerDescription)
                    }
                }
            }
        } catch (e: IOException) {
            closeReason = "连接异常：${e.message}"
        } finally {
            teardownConnection(closeReason)
        }
    }

    /** 会话事件 -> 主线程执行（start/stop 触摸编码器与采集线程，只允许主线程）。 */
    private fun dispatchEvent(event: SessionEvent, peer: String) {
        mainHandler.post {
            when (event) {
                is SessionEvent.StartStreaming -> {
                    negotiatedMediaPort = event.mediaPort
                    negotiatedSampleRate = session?.sampleRate ?: SampleRates.TABLE[SampleRates.DEFAULT_INDEX]
                    framesPerPacket = OpusEncoder.framesOf(negotiatedSampleRate, Defaults.FRAME_MS)
                    startStreaming(peer)
                }
                SessionEvent.StopStreaming -> {
                    stopStreaming("PC 客户端请求停止")
                }
                is SessionEvent.Disconnected -> {
                    teardownConnection("连接断开（reason ${event.reason}）")
                }
            }
        }
    }

    /** 看门狗线程：每 5s 调一次 [WomicSession.checkHeartbeatWatchdog]，超时则关闭 socket。 */
    private fun startWatchdog(sess: WomicSession, client: Socket) {
        val thread = Thread({
            while (!Thread.currentThread().isInterrupted && !client.isClosed) {
                try {
                    Thread.sleep(WATCHDOG_PERIOD_MS)
                } catch (e: InterruptedException) {
                    break
                }
                if (client.isClosed) break
                val alive = sess.checkHeartbeatWatchdog(SystemClock.elapsedRealtime())
                if (!alive) {
                    android.util.Log.w(TAG, "心跳看门狗超时（${WATCHDOG_PERIOD_MS}ms 无 105 心跳），断开连接")
                    try {
                        client.close()
                    } catch (_: IOException) {
                    }
                    break
                }
            }
        }, "womic-watchdog")
        watchdogThread = thread
        thread.isDaemon = true
        thread.start()
    }

    /** 连接级清理：停推流、关 socket、清会话；服务器保持监听。 */
    private fun teardownConnection(reason: String, notify: Boolean = true) {
        stopStreaming(reason)

        val client = connection
        connection = null
        session = null
        negotiatedSampleRate = 0
        negotiatedMediaPort = -1
        peerDescription = ""

        watchdogThread?.interrupt()
        watchdogThread = null
        connThread?.interrupt()
        connThread = null

        if (client != null && !client.isClosed) {
            try {
                client.close()
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "关闭连接异常：${t.message}")
            }
        }
        android.util.Log.i(TAG, "连接已清理：$reason")

        if (notify && isRunning) {
            notifyState(STATE_LISTENING, "等待 PC 连接 · 手机 ${listeningAddress()}")
            updateNotification("等待 PC 连接 · ${listeningAddress()}")
        }
    }

    // ------------------------------------------------------------------
    // 推流启停（START / STOP 事件驱动）
    // ------------------------------------------------------------------

    /** PC 发来 START：开 UDP（协商媒体端口）-> 编码器 -> 采集线程。 */
    private fun startStreaming(peer: String) {
        if (isStreaming) {
            return
        }
        val rate = negotiatedSampleRate
        if (rate !in SampleRates.TABLE) {
            fail("协商采样率非法：$rate（协议表 ${SampleRates.TABLE.toList()}）")
            return
        }
        val port = negotiatedMediaPort
        if (port !in 1..65535) {
            fail("协商媒体端口非法：$port")
            return
        }

        lastError = null
        notifyState(STATE_STARTING, "正在启动推流…")

        // 1) UDP：目标 = PC 客户端地址 + CONFIG 协商的媒体端口
        val sender = UdpSender()
        if (!sender.open(peer, port)) {
            fail(sender.lastError ?: "UDP 打开失败")
            return
        }
        udpSender = sender

        // 2) Opus 编码器（协商采样率，20ms 帧；协议无带内 FEC 通道，关闭）
        val frames = OpusEncoder.framesOf(rate, Defaults.FRAME_MS)
        val enc = OpusEncoder()
        if (!enc.create(
                sampleRate = rate,
                channels = 1,
                bitrateBps = BuildConfig.DEFAULT_BITRATE,
                frameSize = frames,
                complexity = OPUS_COMPLEXITY,
                inbandFec = false
            )
        ) {
            fail("Opus 编码器创建失败：" + (OpusEncoder.loadErrorMessage() ?: "未知原因"))
            return
        }
        encoder = enc
        framesPerPacket = frames

        // 3) AudioRecord（调试音模式跳过，直接合成正弦）
        if (!debugTone) {
            val rec = openRecorder(rate, frames)
            if (rec == null) {
                fail(lastError ?: "AudioRecord 初始化失败（麦克风可能被其他应用独占，或权限未授予）")
                return
            }
            recorder = rec
            actualSampleRate = rec.sampleRate

            try {
                audioManager?.registerAudioDeviceCallback(deviceCallback, mainHandler)
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "注册 AudioDeviceCallback 失败：${t.message}")
            }
        } else {
            actualSampleRate = rate
            android.util.Log.i(TAG, "调试音模式：不打开 AudioRecord，合成 ${TONE_FREQUENCY_HZ}Hz 正弦")
        }

        // 4) 采集线程
        hardwareChanged.set(false)
        val thread = Thread({ captureLoop() }, "womic-capture")
        captureThread = thread
        thread.start()
        isStreaming = true

        notifyState(
            STATE_STREAMING,
            "→ ${peerDescription} · ${sender.targetDescription()} · ${actualSampleRate}Hz"
        )
        updateNotification("正在推流 → ${sender.targetDescription()}")
    }

    /** 停推流：停采集线程、释放 recorder/编码器/UDP；TCP 连接保持。 */
    private fun stopStreaming(reason: String) {
        val thread = captureThread
        captureThread = null
        if (thread != null) {
            thread.interrupt()
            try {
                thread.join(1500)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        isStreaming = false

        releaseRecorder()
        encoder?.destroy()
        encoder = null
        udpSender?.close()
        udpSender = null

        try {
            audioManager?.unregisterAudioDeviceCallback(deviceCallback)
        } catch (t: Throwable) {
            // 未注册时忽略
        }
        android.util.Log.i(TAG, "推流已停止：$reason")
    }

    // ------------------------------------------------------------------
    // 采集主流程
    // ------------------------------------------------------------------

    /** 采集线程主体：阻塞读 -> 编码 -> 封包（格式 C）-> 发送。 */
    private fun captureLoop() {
        val pcm = ShortArray(OpusEncoder.MAX_FRAME)
        val payload = ByteArray(OpusEncoder.MAX_PAYLOAD)

        var seq = 0
        var restarts = 0
        var shortReads = 0L
        var lastLevelPostMs = 0L
        var lastNotifyMs = 0L
        var tonePhase = 0.0
        val toneInc = 2.0 * PI * TONE_FREQUENCY_HZ / actualSampleRate.coerceAtLeast(1)

        while (!Thread.currentThread().isInterrupted) {
            // 硬件变化（插拔耳机/USB 声卡/蓝牙）或采集选项变更（UI 推送）：
            // 重建 AudioRecord（调试音模式没有 AudioRecord，无需处理）
            val deviceChanged = hardwareChanged.getAndSet(false)
            val settingsChanged = captureSettingsChanged.getAndSet(false)
            if ((deviceChanged || settingsChanged) && !debugTone) {
                android.util.Log.i(
                    TAG,
                    if (deviceChanged) {
                        "检测到音频设备变化，重建 AudioRecord"
                    } else {
                        "采集选项变更，重建 AudioRecord"
                    }
                )
                releaseRecorder()
                SystemClock.sleep(RESTART_BACKOFF_MS)
                val rec = openRecorder(negotiatedSampleRate, framesPerPacket)
                if (rec == null) {
                    val reason = if (settingsChanged) {
                        "应用新采集选项时 AudioRecord 重建失败：${lastError ?: "未知原因"}"
                    } else {
                        "音频设备变化后无法重新初始化 AudioRecord"
                    }
                    mainHandler.post { fail(reason) }
                    return
                }
                recorder = rec
                continue
            }

            val rec = recorder
            val read: Int
            if (debugTone || rec == null) {
                // 调试音：合成 440Hz 正弦，按帧长睡眠模拟实时节奏
                var i = 0
                while (i < framesPerPacket) {
                    pcm[i] = (sin(tonePhase) * TONE_AMPLITUDE * 32767.0).toInt().toShort()
                    tonePhase += toneInc
                    if (tonePhase > 2.0 * PI) tonePhase -= 2.0 * PI
                    i++
                }
                read = framesPerPacket
                SystemClock.sleep(Defaults.FRAME_MS.toLong())
            } else {
                // 阻塞读取：没有数据时线程挂起，不占用 CPU
                read = rec.read(pcm, 0, framesPerPacket)
            }

            if (read == framesPerPacket) {
                restarts = 0

                // 电平
                val now = SystemClock.elapsedRealtime()
                if (now - lastLevelPostMs >= LEVEL_INTERVAL_MS) {
                    lastLevelPostMs = now
                    val level = LevelMeter.analyze(pcm, 0, framesPerPacket)
                    mainHandler.post { listener?.onLevel(level.rmsDb, level.peakDb) }
                }

                // 编码
                val payloadLen = encoder?.encode(pcm, payload) ?: -1
                if (payloadLen <= 0) {
                    android.util.Log.w(TAG, "编码失败：$payloadLen，跳过本帧")
                    continue
                }

                // 封包（WO Mic 格式 C：11 字节头 + payload，规格 v1 第 3 节）
                // 时间戳用采集侧单调时钟（elapsedRealtime 毫秒），seq 每真实帧 +1。
                val ts = SystemClock.elapsedRealtime()
                val frameSeq = seq
                val packet = WomicPacketizer.build(frameSeq, ts, payload.copyOf(payloadLen), volume)
                seq = (seq + 1) and 0xFFFF

                // 发送
                val sender = udpSender
                if (sender == null) {
                    mainHandler.post { fail("UDP sender 为空") }
                    return
                }
                // 发送（逻辑不变：仍只调一次 send，仅把返回值留给下面的埋点分支）
                val sent = sender.send(packet, packet.size)
                if (!sent) {
                    if (sender.failureStreak >= MAX_SEND_FAILURES) {
                        mainHandler.post {
                            fail("连续 ${sender.failureStreak} 次发送失败，已停止：${sender.lastError}")
                        }
                        return
                    }
                } else if (frameSeq == 0) {
                    // 诊断埋点（每个会话仅一条，不逐包打）：C05 联调失败时可直接从
                    // logcat 确认"app 到底发没发首包"，与 pc_client.py C05 失败路径
                    // 的 nc/ps、/proc/net/udp 抓取互补，区分 app 侧未发包与 tap 侧
                    // 未收到。调试音模式同样经过这里（该模式不打开 AudioRecord）。
                    android.util.Log.i(
                        TAG,
                        "首个 UDP 包已发送 seq=$frameSeq ts=$ts 目标=${sender.targetDescription()}"
                    )
                }

                if (now - lastNotifyMs >= NOTIFY_INTERVAL_MS) {
                    lastNotifyMs = now
                    updateNotification("正在推流 → ${sender.targetDescription()} · ${sender.packetsSent} 包")
                }
                continue
            }

            if (debugTone || rec == null) {
                continue
            }

            when (read) {
                AudioRecord.ERROR_INVALID_OPERATION -> {
                    mainHandler.post { fail("AudioRecord 状态错误（ERROR_INVALID_OPERATION）：录音被系统回收") }
                    return
                }
                AudioRecord.ERROR_BAD_VALUE -> {
                    mainHandler.post { fail("AudioRecord 参数错误（ERROR_BAD_VALUE）") }
                    return
                }
                AudioRecord.ERROR_DEAD_OBJECT -> {
                    // 典型：另一个应用抢占了麦克风 / 音频服务重启 / 蓝牙设备断开
                    restarts++
                    android.util.Log.w(TAG, "ERROR_DEAD_OBJECT，重建 AudioRecord（第 $restarts 次）")
                    if (restarts > MAX_RESTARTS) {
                        mainHandler.post { fail("AudioRecord 连续 $restarts 次失效（麦克风可能被其他应用独占）") }
                        return
                    }
                    releaseRecorder()
                    SystemClock.sleep(RESTART_BACKOFF_MS * restarts)
                    val newRec = openRecorder(negotiatedSampleRate, framesPerPacket)
                    if (newRec == null) {
                        mainHandler.post { fail("重建 AudioRecord 失败") }
                        return
                    }
                    recorder = newRec
                }
                AudioRecord.ERROR -> {
                    android.util.Log.w(TAG, "AudioRecord 通用错误，重试")
                    SystemClock.sleep(RESTART_BACKOFF_MS)
                }
                else -> {
                    // 0 < read < framesPerPacket：短读。首帧常见，攒够一帧再编码更稳。
                    shortReads++
                    if (shortReads % 100L == 0L) {
                        android.util.Log.w(TAG, "累计短读 $shortReads 次（read=$read < $framesPerPacket）")
                    }
                }
            }
        }

        android.util.Log.i(TAG, "采集线程退出")
    }

    // ------------------------------------------------------------------
    // AudioRecord
    // ------------------------------------------------------------------

    /**
     * 打开 AudioRecord。
     *
     * 音源默认 [AudioSourceIds.DEFAULT]（= `MediaRecorder.AudioSource.DEFAULT`），
     * 与官方 app 默认值一致（证据 V1/f.java：audioSource=0），用户可在"采集设置"
     * 里改选其他档位（含需设备支持的 UNPROCESSED，读取时已清洗回落）。
     * 选档理由：WO Mic 是单向推流（手机麦克风 -> PC），手机上不存在回声环路，
     * 无需 VOICE_COMMUNICATION 的 AEC 通路；且 DEFAULT 不会像
     * VOICE_COMMUNICATION 那样在部分设备上强制降到 16kHz。
     *
     * 输入设备：用户在"采集设置"里选定后经 [CaptureSettings.micDeviceId] 传入，
     * 由 [applyPreferredDevice] 在 startRecording 之前应用；未选（自动）时
     * 不调用 setPreferredDevice，与改动前行为一致。
     *
     * 系统效果（NS/AEC/AGC）：录音开始后由 [attachSystemEffects] 绑定到本会话，
     * 全部由 framework 效果类实现（零自研算法）。
     *
     * 采样率：按协商值申请；部分设备仍会强制调整，此时以 [AudioRecord.getSampleRate]
     * 的**实际值**为准重新推导编码器参数并继续推流（PC 端按协商率解码，极端情况下
     * 会变调，但好过直接失败；日志会明确告警）。
     *
     * @return 成功返回 AudioRecord；失败返回 null 并置 [lastError]
     */
    private fun openRecorder(sampleRate: Int, frameSize: Int): AudioRecord? {
        val settings = captureSettings
        val minBuffer = AudioRecord.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        if (minBuffer <= 0) {
            lastError = "该设备不支持 ${sampleRate}Hz 单声道 16bit 采集（getMinBufferSize=$minBuffer）"
            android.util.Log.e(TAG, lastError!!)
            return null
        }
        // 至少 2 倍最小缓冲，并保证 >= 4 帧，降低欠载概率
        val bufferSize = maxOf(minBuffer * 2, frameSize * 2 * 4)

        val rec = try {
            AudioRecord.Builder()
                .setAudioSource(settings.audioSource)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(sampleRate)
                        .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build()
                )
                .setBufferSizeInBytes(bufferSize)
                .build()
        } catch (t: Throwable) {
            lastError = "AudioRecord 构建异常：${t.message}"
            android.util.Log.e(TAG, lastError!!, t)
            return null
        }

        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            lastError = "AudioRecord 初始化失败：麦克风可能被其他应用独占，或未授予 RECORD_AUDIO 权限"
            try {
                rec.release()
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "release 异常：${t.message}")
            }
            android.util.Log.e(TAG, lastError!!)
            return null
        }

        // 输入设备偏好：必须在 startRecording 之前设置（setPreferredDevice 对已
        // 开始录音的会话是否生效依设备而定，不赌）。失败不中止推流：回退系统
        // 默认设备并给用户明确提示（见 [applyPreferredDevice]）。
        applyPreferredDevice(rec)

        val actual = rec.sampleRate
        if (actual != sampleRate) {
            android.util.Log.w(TAG, "采样率被平台调整：请求 $sampleRate -> 实际 $actual，按实际值重建编码器")
        }
        actualSampleRate = actual

        // 实际采样率与请求值不同时，必须同步帧长与编码器，否则 Opus 帧长非法
        if (actual != sampleRate) {
            val newFrames = OpusEncoder.framesOf(actual, Defaults.FRAME_MS)
            if (!OpusEncoder.isValidFrame(actual, newFrames)) {
                lastError = "平台返回采样率 ${actual}Hz 与帧长 ${Defaults.FRAME_MS}ms 组合非法"
                try {
                    rec.release()
                } catch (t: Throwable) {
                    android.util.Log.w(TAG, "release 异常：${t.message}")
                }
                return null
            }
            framesPerPacket = newFrames
            encoder?.destroy()
            val enc = OpusEncoder()
            if (!enc.create(
                    sampleRate = actual,
                    channels = 1,
                    bitrateBps = BuildConfig.DEFAULT_BITRATE,
                    frameSize = newFrames,
                    complexity = OPUS_COMPLEXITY,
                    inbandFec = false
                )
            ) {
                lastError = "按实际采样率 ${actual}Hz 重建编码器失败"
                try {
                    rec.release()
                } catch (t: Throwable) {
                    android.util.Log.w(TAG, "release 异常：${t.message}")
                }
                return null
            }
            encoder = enc
        }

        try {
            rec.startRecording()
        } catch (t: IllegalStateException) {
            lastError = "startRecording 失败：${t.message}"
            try {
                rec.release()
            } catch (ignore: Throwable) {
                // ignore
            }
            return null
        }
        if (rec.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
            lastError = "录音未真正开始（recordingState=${rec.recordingState}）"
            try {
                rec.release()
            } catch (ignore: Throwable) {
                // ignore
            }
            return null
        }

        // 系统效果（NS/AEC/AGC）绑定本次录音会话；失败只提示不中止
        attachSystemEffects(rec)
        return rec
    }

    /**
     * 应用用户选择的输入设备（`AudioRecord.setPreferredDevice`，API 23+）。
     *
     * 策略（不静默失败）：
     *  - 自动（[CaptureSettings.MIC_DEVICE_AUTO]）：清除既有偏好后交还系统路由；
     *  - 设备已不存在（拔出）：提示 + 回退系统默认设备；
     *  - setPreferredDevice 返回 false（设备失效/本会话不支持）：提示 + 回退。
     * 两种情况都**继续推流**：断链比用错设备更糟，用户提示里已说明实际在用默认设备。
     *
     * @return 成功时返回生效的设备，否则 null。
     */
    private fun applyPreferredDevice(rec: AudioRecord): AudioDeviceInfo? {
        val settings = captureSettings
        if (settings.micDeviceId == CaptureSettings.MIC_DEVICE_AUTO) {
            try {
                rec.setPreferredDevice(null)
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "清除设备偏好失败：${t.message}")
            }
            return null
        }
        val device = deviceManager.deviceById(settings.micDeviceId)
        if (device == null) {
            postMessage(getString(R.string.capture_msg_device_missing))
            android.util.Log.w(TAG, "所选输入设备当前不可用：id=${settings.micDeviceId}，回退系统默认")
            return null
        }
        val applied = try {
            rec.setPreferredDevice(device)
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "setPreferredDevice 异常：${t.message}")
            false
        }
        if (!applied) {
            postMessage(
                getString(
                    R.string.capture_msg_device_switch_failed,
                    AudioDeviceManager.deviceLabel(device)
                )
            )
            android.util.Log.w(
                TAG,
                "setPreferredDevice 返回 false：${AudioDeviceManager.deviceLabel(device)}，回退系统默认"
            )
            return null
        }
        android.util.Log.i(TAG, "已偏好输入设备：${AudioDeviceManager.deviceLabel(device)}")
        return device
    }

    /** 按当前采集选项把系统效果绑到录音会话（无效果开关时释放既有对象）。 */
    private fun attachSystemEffects(rec: AudioRecord) {
        // 防御：无论调用方是否已释放，先清既有对象，避免"忘了先 release"的泄漏路径
        releaseSystemEffects()
        val settings = captureSettings
        if (!settings.hasAnyEffect()) {
            return
        }
        val effects = SystemAudioEffects()
        effects.attach(
            sessionId = rec.audioSessionId,
            settings = settings
        ) { name, reason ->
            postMessage(getString(R.string.capture_msg_effect_failed, name, reason))
        }
        audioEffects = effects
        val active = effects.describe()
        android.util.Log.i(
            TAG,
            "系统效果已绑定会话 ${rec.audioSessionId}：${active.ifEmpty { "（无效果生效）" }}"
        )
    }

    /** 释放系统效果（必须在 AudioRecord.release 之前；可重复调用）。 */
    private fun releaseSystemEffects() {
        val effects = audioEffects
        audioEffects = null
        effects?.release()
    }

    /** 非致命提示 -> 主线程 listener（服务继续运行）。 */
    private fun postMessage(message: String) {
        mainHandler.post { listener?.onMessage(message) }
    }

    private fun releaseRecorder() {
        // 效果持有录音会话：必须先于 recorder.release() 释放，否则静默泄漏
        releaseSystemEffects()
        val rec = recorder
        recorder = null
        if (rec == null) {
            return
        }
        try {
            if (rec.recordingState == AudioRecord.RECORDSTATE_RECORDING) {
                rec.stop()
            }
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "stop 异常：${t.message}")
        }
        try {
            rec.release()
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "release 异常：${t.message}")
        }
    }

    // ------------------------------------------------------------------
    // WakeLock
    // ------------------------------------------------------------------
    private fun acquireWakeLock() {
        if (wakeLock != null) {
            return
        }
        val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager
        val lock = pm?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "WomicWifiClient:MicCapture")
        if (lock != null) {
            try {
                // 10 小时兜底，正常停止时会显式 release
                lock.acquire(10 * 60 * 60 * 1000L)
                wakeLock = lock
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "获取 WakeLock 失败：${t.message}")
            }
        }
    }

    private fun releaseWakeLock() {
        val lock = wakeLock
        wakeLock = null
        if (lock != null && lock.isHeld) {
            try {
                lock.release()
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "释放 WakeLock 失败：${t.message}")
            }
        }
    }

    // ------------------------------------------------------------------
    // 前台服务类型降级链
    // ------------------------------------------------------------------

    /**
     * Android 14+ 对 microphone 型前台服务有 while-in-use 权限校验。
     * 拿不到 RECORD_AUDIO 时（例如只用调试音验证链路），按
     * microphone -> 0（无类型）的顺序降级，全部失败才返回 false。
     */
    private fun startForegroundSafely(): Boolean {
        val notification = buildNotification("正在启动…")
        val attempts = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            listOf(ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE, 0)
        } else {
            listOf(0)
        }
        for (type in attempts) {
            try {
                ServiceCompat.startForeground(this, NOTIFICATION_ID, notification, type)
                return true
            } catch (t: Throwable) {
                android.util.Log.w(TAG, "startForeground(type=$type) 失败：${t.message}")
            }
        }
        return false
    }

    // ------------------------------------------------------------------
    // 通知
    // ------------------------------------------------------------------
    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
            if (manager != null && manager.getNotificationChannel(CHANNEL_ID) == null) {
                val channel = NotificationChannel(
                    CHANNEL_ID,
                    CHANNEL_NAME,
                    NotificationManager.IMPORTANCE_LOW
                ).apply {
                    description = "WO Mic 采集推流正在运行时显示"
                    setShowBadge(false)
                }
                manager.createNotificationChannel(channel)
            }
        }
    }

    private fun buildNotification(text: String): Notification {
        val contentIntent = PendingIntent.getActivity(
            this,
            1,
            Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or pendingIntentImmutableFlag()
        )

        val stopIntent = PendingIntent.getService(
            this,
            2,
            buildStopIntent(this),
            PendingIntent.FLAG_UPDATE_CURRENT or pendingIntentImmutableFlag()
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_stat_mic)
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .addAction(
                R.drawable.ic_stat_mic,
                getString(R.string.notification_action_stop),
                stopIntent
            )
            .build()
    }

    private fun updateNotification(text: String) {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
        try {
            manager.notify(NOTIFICATION_ID, buildNotification(text))
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "更新通知失败：${t.message}")
        }
    }

    private fun pendingIntentImmutableFlag(): Int {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            PendingIntent.FLAG_IMMUTABLE
        } else {
            0
        }
    }

    // ------------------------------------------------------------------
    // 回调
    // ------------------------------------------------------------------
    private fun notifyState(state: String, detail: String) {
        mainHandler.post {
            listener?.onState(state, detail)
        }
    }
}
