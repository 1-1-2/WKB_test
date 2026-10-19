package com.womic.open

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
import android.media.MediaRecorder
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import java.util.concurrent.atomic.AtomicBoolean

/**
 * MicCaptureService -- 采集 + 编码 + 发送的前台服务。
 *
 * 链路：
 * ```
 * AudioRecord(VOICE_COMMUNICATION, PCM16 单声道)
 *   -> 采集线程阻塞 read(frameSize)
 *   -> OpusEncoder.encode()
 *   -> Packetizer.pack(32B 头, 大端)
 *   -> UdpSender.send()  ---- UDP ---->  Windows womicserver
 * ```
 *
 * 关键设计：
 *   1) **前台服务**：保证息屏/切后台后仍持续采集，通知栏提供"停止"入口；
 *   2) **PARTIAL_WAKE_LOCK**：防止息屏后 CPU 休眠导致采集线程被拖慢；
 *   3) **独立采集线程 + 阻塞 read**（不用轮询）：read() 在数据不足时阻塞，CPU 占用最低；
 *   4) **硬件热切换 / ERROR_DEAD_OBJECT 自愈**：AudioDeviceCallback 置位后重建 AudioRecord，
 *      read 返回 ERROR_DEAD_OBJECT 时按退避重建，超过阈值才报错停止；
 *   5) **音频源用 VOICE_COMMUNICATION**：复用平台既有的 AEC/NS 通路。
 *      **本项目明确不自研 AEC/NS**（见 PRD 非目标），选 MIC 会绕过这套处理。
 */
class MicCaptureService : Service() {

    /** UI 侧状态回调（全部在主线程执行）。 */
    interface CaptureStatusListener {
        /** 状态变化：state 见 [STATE_IDLE] / [STATE_STARTING] / [STATE_RUNNING] / [STATE_STOPPED] / [STATE_ERROR]。 */
        fun onState(state: String, detail: String)

        /** 电平更新（约 20 次/秒）。 */
        fun onLevel(rmsDb: Float, peakDb: Float)

        /** 出错（服务会自行停止）。 */
        fun onError(message: String)
    }

    companion object {
        private const val TAG = "MicCaptureService"

        const val ACTION_START = "com.womic.open.action.START"
        const val ACTION_STOP = "com.womic.open.action.STOP"

        const val EXTRA_HOST = "extra.host"
        const val EXTRA_PORT = "extra.port"
        const val EXTRA_SAMPLE_RATE = "extra.sample_rate"
        const val EXTRA_FRAME_MS = "extra.frame_ms"
        const val EXTRA_BITRATE = "extra.bitrate"
        const val EXTRA_FEC = "extra.fec"

        const val CHANNEL_ID = "womic_capture"
        const val CHANNEL_NAME = "麦克风采集"
        const val NOTIFICATION_ID = 0x574F4D50.toInt()

        const val STATE_IDLE = "空闲"
        const val STATE_STARTING = "启动中"
        const val STATE_RUNNING = "采集中"
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

        /** 服务是否正在采集（供 UI 在 onResume 时同步按钮状态）。 */
        @Volatile
        var isRunning: Boolean = false

        /** 最后一次错误原因（UI 显示用）。 */
        @Volatile
        var lastError: String? = null

        /** 最近一次采集的实际采样率（可能与请求值不同，见 [MicCaptureService.openRecorder]）。 */
        @Volatile
        var actualSampleRate: Int = 0

        /**
         * 构造启动 Intent（建议用本方法，避免漏传参数）。
         */
        fun buildStartIntent(context: Context, config: Prefs.Config): Intent {
            return Intent(context, MicCaptureService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_HOST, config.host)
                putExtra(EXTRA_PORT, config.port)
                putExtra(EXTRA_SAMPLE_RATE, config.sampleRate)
                putExtra(EXTRA_FRAME_MS, config.frameMs)
                putExtra(EXTRA_BITRATE, config.bitrateBps)
                putExtra(EXTRA_FEC, config.fecEnabled)
            }
        }

        fun buildStopIntent(context: Context): Intent {
            return Intent(context, MicCaptureService::class.java).apply {
                action = ACTION_STOP
            }
        }
    }

    // ------------------------------------------------------------------
    // 字段
    // ------------------------------------------------------------------
    private val binder = LocalBinder()
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    private var listener: CaptureStatusListener? = null

    @Volatile
    private var captureThread: Thread? = null

    @Volatile
    private var recorder: AudioRecord? = null

    // encoder / udpSender 由主线程（startCapture / stopCapture）创建与释放，
    // 由采集线程读写，必须 volatile 才能保证跨线程可见性。
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

    // 运行期参数
    //
    // 全部 @Volatile：主线程 onStartCommand 写，采集线程读（部分还由采集线程在
    // 设备重建路径里写，见 openRecorder 对 framesPerPacket 的改写）。
    // 没有 volatile 时采集线程可能一直读到旧帧长，导致 AudioRecord.read 的参数
    // 与编码器帧长不一致（缺陷 D-03）。
    @Volatile
    private var host: String = ""
    @Volatile
    private var port: Int = Prefs.DEFAULT_PORT
    @Volatile
    private var requestedSampleRate: Int = Prefs.DEFAULT_SAMPLE_RATE
    @Volatile
    private var frameMs: Int = Prefs.DEFAULT_FRAME_MS
    @Volatile
    private var bitrateBps: Int = Prefs.DEFAULT_BITRATE_BPS
    @Volatile
    private var fecEnabled: Boolean = true
    @Volatile
    private var framesPerPacket: Int = OpusEncoder.framesOf(Prefs.DEFAULT_SAMPLE_RATE, Prefs.DEFAULT_FRAME_MS)

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
                host = intent.getStringExtra(EXTRA_HOST) ?: ""
                port = intent.getIntExtra(EXTRA_PORT, Prefs.DEFAULT_PORT)
                requestedSampleRate = intent.getIntExtra(EXTRA_SAMPLE_RATE, Prefs.DEFAULT_SAMPLE_RATE)
                frameMs = intent.getIntExtra(EXTRA_FRAME_MS, Prefs.DEFAULT_FRAME_MS)
                bitrateBps = intent.getIntExtra(EXTRA_BITRATE, Prefs.DEFAULT_BITRATE_BPS)
                fecEnabled = intent.getBooleanExtra(EXTRA_FEC, true)
                framesPerPacket = OpusEncoder.framesOf(requestedSampleRate, frameMs)
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
        isRunning -> STATE_RUNNING
        lastError != null -> STATE_ERROR
        else -> STATE_IDLE
    }

    /** 当前状态细节（目标地址 + 统计）。 */
    fun currentDetail(): String {
        val sender = udpSender
        return if (isRunning && sender != null) {
            "${sender.targetDescription()} · ${sender.packetsSent} 包 · ${actualSampleRate}Hz"
        } else {
            lastError ?: "未采集"
        }
    }

    /** 已发送包数（UI 轮询显示）。 */
    fun packetsSent(): Long = udpSender?.packetsSent ?: 0L

    /** 已发送字节数。 */
    fun bytesSent(): Long = udpSender?.bytesSent ?: 0L

    /** 连续发送失败次数。 */
    fun sendFailures(): Int = udpSender?.failureStreak ?: 0

    // ------------------------------------------------------------------
    // 采集主流程
    // ------------------------------------------------------------------

    /** 启动采集：先上前台通知，再初始化各组件，最后起线程。 */
    private fun startCapture() {
        if (isRunning) {
            return
        }
        lastError = null
        actualSampleRate = 0
        notifyState(STATE_STARTING, "正在初始化…")

        // 1) 前台通知：必须在 startForegroundService 之后尽快调用，否则会被系统判为 ANR/崩溃
        try {
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                buildNotification("正在启动…"),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                } else {
                    0
                }
            )
        } catch (t: Throwable) {
            fail("无法启动前台服务：${t.message}（请确认已授予通知权限）")
            return
        }

        // 2) 参数校验
        if (host.isBlank()) {
            fail("目标 IP 为空")
            return
        }
        if (port !in 1..65535) {
            fail("端口非法：$port")
            return
        }
        if (requestedSampleRate !in Prefs.SUPPORTED_RATES) {
            fail("采样率不支持：$requestedSampleRate（服务端支持 16000/44100/48000，Opus 只能用 16000/48000）")
            return
        }
        if (!OpusEncoder.isValidFrame(requestedSampleRate, framesPerPacket)) {
            fail("帧长不是 2.5ms 整数倍：${requestedSampleRate}Hz / ${frameMs}ms")
            return
        }

        // 3) WakeLock：息屏后保持 CPU 运行
        acquireWakeLock()

        // 4) UDP
        val sender = UdpSender()
        if (!sender.open(host, port)) {
            fail(sender.lastError ?: "UDP 打开失败")
            releaseWakeLock()
            return
        }
        udpSender = sender

        // 5) Opus 编码器
        val enc = OpusEncoder()
        if (!enc.create(
                sampleRate = requestedSampleRate,
                channels = 1,
                bitrateBps = bitrateBps,
                frameSize = framesPerPacket,
                complexity = 5,
                inbandFec = fecEnabled
            )
        ) {
            fail("Opus 编码器创建失败：" + (OpusEncoder.loadErrorMessage() ?: "未知原因"))
            releaseWakeLock()
            return
        }
        encoder = enc

        // 6) AudioRecord
        val rec = openRecorder(requestedSampleRate, framesPerPacket)
        if (rec == null) {
            fail(lastError ?: "AudioRecord 初始化失败（麦克风可能被其他应用独占，或权限未授予）")
            releaseWakeLock()
            return
        }
        recorder = rec

        // 7) 硬件热切换监听
        try {
            audioManager?.registerAudioDeviceCallback(deviceCallback, mainHandler)
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "注册 AudioDeviceCallback 失败：${t.message}")
        }

        // 8) 采集线程
        isRunning = true
        hardwareChanged.set(false)
        val thread = Thread({ captureLoop() }, "womic-capture")
        captureThread = thread
        thread.start()

        notifyState(STATE_RUNNING, "已连接 ${sender.targetDescription()} · ${actualSampleRate}Hz")
        updateNotification("正在采集 → ${sender.targetDescription()}")
    }

    /** 停止采集并释放全部资源（可重复调用）。 */
    private fun stopCapture(state: String, reason: String) {
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

        try {
            audioManager?.unregisterAudioDeviceCallback(deviceCallback)
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "注销 AudioDeviceCallback 失败：${t.message}")
        }

        releaseRecorder()
        encoder?.destroy()
        encoder = null
        udpSender?.close()
        udpSender = null
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

    /** 采集线程主体：阻塞读 -> 编码 -> 封包 -> 发送。 */
    private fun captureLoop() {
        // 按 Opus 单帧上限分配：平台把采样率改成 48k 后 framesPerPacket 会变大，
        // 若按启动时大小分配，重建 AudioRecord 后 read() 会越界。
        val pcm = ShortArray(OpusEncoder.MAX_FRAME)
        val payload = ByteArray(Packetizer.MAX_PAYLOAD)
        val redundant = ByteArray(Packetizer.MAX_PAYLOAD)
        val packet = ByteArray(Packetizer.MAX_PACKET_SIZE)

        var seq = 0
        var restarts = 0
        var shortReads = 0L
        var lastLevelPostMs = 0L
        var lastNotifyMs = 0L
        val startTimeMs = SystemClock.elapsedRealtime()

        while (!Thread.currentThread().isInterrupted) {
            // 硬件变化（插拔耳机/USB 声卡/蓝牙）：重建 AudioRecord
            if (hardwareChanged.getAndSet(false)) {
                android.util.Log.i(TAG, "检测到音频设备变化，重建 AudioRecord")
                releaseRecorder()
                SystemClock.sleep(RESTART_BACKOFF_MS)
                val rec = openRecorder(requestedSampleRate, framesPerPacket)
                if (rec == null) {
                    mainHandler.post { fail("音频设备变化后无法重新初始化 AudioRecord") }
                    return
                }
                recorder = rec
                continue
            }

            val rec = recorder
            if (rec == null) {
                mainHandler.post { fail("AudioRecord 已释放") }
                return
            }

            // 阻塞读取：没有数据时线程挂起，不占用 CPU
            val read = rec.read(pcm, 0, framesPerPacket)

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

                // 封包（32 字节头 + payload，大端）
                val ts = SystemClock.elapsedRealtime() - startTimeMs
                // 本帧占用的序号。**只有真实帧才推进序号**：FEC 冗余包是它的
                // 第二份拷贝，复用同一个 seq，绝不能占用新序号。
                val frameSeq = seq
                val total = Packetizer.pack(
                    dst = packet,
                    seq = frameSeq,
                    timestampMs = ts,
                    sampleRate = actualSampleRate,
                    channels = 1,
                    frames = framesPerPacket,
                    payload = payload,
                    payloadLen = payloadLen,
                    flags = 0
                )
                seq++

                // 发送
                val sender = udpSender
                if (sender == null) {
                    mainHandler.post { fail("UDP sender 为空") }
                    return
                }
                if (!sender.send(packet, total)) {
                    if (sender.failureStreak >= MAX_SEND_FAILURES) {
                        mainHandler.post {
                            fail("连续 ${sender.failureStreak} 次发送失败，已停止：${sender.lastError}")
                        }
                        return
                    }
                } else if (fecEnabled && ((frameSeq + 1) and 0x0F) == 0) {
                    // 每 16 个**真实帧**（frameSeq = 15, 31, 47, …）附一份刚刚那
                    // 一帧的冗余重发包（带内 FEC），服务端若发现原包丢失可用它补洞。
                    // 失败不影响主链路。
                    //
                    // !!! 必须复用 frameSeq，且**不要** seq++ !!!
                    // 带内 FEC 的语义是"同一个 seq 的第二份拷贝"：服务端据此判定
                    // 为重复包（原包已到则丢弃，原包丢失则补洞）。若给冗余包分配
                    // 新 seq，服务端无从分辨它和新帧，只能当成新的一帧注入 ——
                    // 于是每 16 帧多产出 20ms，播放速率快 6.25%，抖动缓冲被持续
                    // 积压直至驱逐丢帧（缺陷 D-01）。
                    val redLen = encoder?.plc(redundant) ?: 0
                    if (redLen > 0) {
                        val redTotal = Packetizer.pack(
                            dst = packet,
                            seq = frameSeq,
                            timestampMs = ts,
                            sampleRate = actualSampleRate,
                            channels = 1,
                            frames = framesPerPacket,
                            payload = redundant,
                            payloadLen = redLen,
                            flags = Packetizer.FLAG_FEC
                        )
                        sender.send(packet, redTotal)
                    }
                }

                if (now - lastNotifyMs >= NOTIFY_INTERVAL_MS) {
                    lastNotifyMs = now
                    updateNotification("正在采集 → ${sender.targetDescription()} · ${sender.packetsSent} 包")
                }
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
                    val newRec = openRecorder(requestedSampleRate, framesPerPacket)
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
                    // 这里简单跳过；若长期短读说明底层 buffer 配置异常。
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
     * 音频源固定为 [MediaRecorder.AudioSource.VOICE_COMMUNICATION]：
     * 复用平台既有的回声消除/噪声抑制通路；本项目**不自研 AEC/NS**（PRD 非目标）。
     *
     * 采样率：按 [sampleRate] 申请；部分设备（尤其用 VOICE_COMMUNICATION 时）会强制
     * 降到 16 kHz，此时以 [AudioRecord.getSampleRate] 的**实际值**为准重新推导编码器参数，
     * 并在 UI 上提示——服务端会在 capture_rate != decode_rate 时自动重采样（不致命），
     * 但端点率与解码率不一致会被服务端拒绝启动（见 shared/core womic_sample_rate.h）。
     *
     * @return 成功返回 AudioRecord；失败返回 null 并置 [lastError]
     */
    private fun openRecorder(sampleRate: Int, frameSize: Int): AudioRecord? {
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
                .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
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

        val actual = rec.sampleRate
        if (actual != sampleRate) {
            android.util.Log.w(TAG, "采样率被平台调整：请求 $sampleRate -> 实际 $actual，按实际值重建编码器")
        }
        actualSampleRate = actual

        // 实际采样率与请求值不同时，必须同步帧长与编码器，否则 Opus 帧长非法
        if (actual != requestedSampleRate) {
            val newFrames = OpusEncoder.framesOf(actual, frameMs)
            if (!OpusEncoder.isValidFrame(actual, newFrames)) {
                lastError = "平台返回采样率 ${actual}Hz 与帧长 ${frameMs}ms 组合非法"
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
                    bitrateBps = bitrateBps,
                    frameSize = framesPerPacket,
                    complexity = 5,
                    inbandFec = fecEnabled
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
        return rec
    }

    private fun releaseRecorder() {
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
        val lock = pm?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "WomicOpen:MicCapture")
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
                    description = "Womic 麦克风采集正在运行时显示"
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
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
            ?: return
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
