package com.miclink.open

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import com.miclink.open.theme.AppPalette
import com.miclink.open.theme.ThemeMode
import com.miclink.open.theme.ThemeState

/**
 * 主界面状态：配置 + 服务状态 + 统计 + 电平。
 *
 * 全部由 [MainActivity] 从服务回调 / SharedPreferences 灌入；
 * Compose 侧只读 [uiState]，不直接碰服务（关注点分离，旋转屏后状态不丢）。
 */
data class MainUiState(
    // 可编辑配置（启动服务时锁定）
    val controlPortText: String = Prefs.DEFAULT_CONTROL_PORT.toString(),
    val volume: Int = Prefs.DEFAULT_VOLUME,
    val debugTone: Boolean = false,
    // 采集选项（音源 / 麦克风 / 系统效果）：与端口不同，录音中也可改，即时生效
    val capture: CaptureSettings = CaptureSettings(),
    // 当前实际生效的输入设备（AudioRecord.getRoutedDevice；未录音为 null）
    val routedDeviceLabel: String? = null,
    // 服务状态
    val state: String = MicCaptureService.STATE_IDLE,
    val detail: String = "",
    val listeningAddress: String = "",
    // 统计（轮询）
    val packets: Long = 0L,
    val bytes: Long = 0L,
    val failures: Int = 0,
    // 电平
    val rmsDb: Float = LevelMeter.SILENCE_DB,
    val peakDb: Float = LevelMeter.SILENCE_DB,
    // 错误提示（一次性事件，UI 展示后应调 [consumeMessage]）
    val error: String? = null,
    // 非致命提示（一次性事件，服务仍在运行；与 error 共用同一弹窗，error 优先）
    val message: String? = null
) {
    val isRunning: Boolean get() = MicCaptureService.isRunning
    val isStreaming: Boolean get() = MicCaptureService.isStreaming
}

/**
 * MainViewModel -- 主界面的唯一状态容器。
 *
 * 不持有 Service 引用（避免泄漏）：Activity 在 onStart 绑定服务后，
 * 通过 [onServiceState] / [onLevel] / [pollStats] 把状态推进来。
 * 用 Compose [mutableStateOf] 而非 StateFlow：单屏应用无需引入协程依赖。
 */
class MainViewModel : ViewModel() {

    var uiState: MainUiState by mutableStateOf(MainUiState())
        private set

    /**
     * 主题设置（与运行参数分离）：
     * 运行参数是"启动服务前锁定"的编辑态，主题是"改完立刻生效并落盘"的偏好态，
     * 两者生命周期不同，混在一个 state 里会让"什么时候该写盘"变糊。
     */
    var themeState: ThemeState by mutableStateOf(ThemeState())
        private set

    /** 主题存储（[loadTheme] 时保留引用；Prefs 持有 applicationContext，无泄漏风险）。 */
    private var themeStore: Prefs? = null

    /**
     * 配置存储（[loadConfig] 时保留引用）：采集选项每次变更即时落盘用。
     * 与 [themeStore] 分开命名：主题与运行配置的写盘时机不同（见类注释）。
     */
    private var configStore: Prefs? = null

    /** 有待推送给运行中服务的采集选项（下次 [pushCaptureSettings] 时同步）。 */
    private var capturePushPending = false

    /** Activity 创建时从 Prefs 灌入可编辑配置。 */
    fun loadConfig(prefs: Prefs) {
        configStore = prefs
        val config = prefs.load()
        uiState = uiState.copy(
            controlPortText = config.controlPort.toString(),
            volume = config.volume,
            debugTone = config.debugTone,
            capture = config.capture
        )
    }

    /**
     * Activity 创建时灌入主题设置，并保留存储引用供后续修改落盘。
     *
     * 与 [loadConfig] 的差别：配置只在点"启动服务"时保存，主题每次变更都即时保存
     * （验收 #3：重启保持），所以这里必须留住 [Prefs]（经它委托 ThemePrefs）。
     */
    fun loadTheme(prefs: Prefs) {
        themeStore = prefs
        themeState = prefs.loadTheme()
    }

    /** 把当前 UI 配置读成 [Prefs.Config]（端口文本非法时返回 null）。 */
    fun readConfig(): Prefs.Config? {
        val port = uiState.controlPortText.trim().toIntOrNull() ?: return null
        if (port !in 1..65535) return null
        return Prefs.Config(
            controlPort = port,
            volume = uiState.volume,
            debugTone = uiState.debugTone,
            capture = uiState.capture
        )
    }

    fun onControlPortTextChanged(text: String) {
        // 只允许数字，避免非法字符进入配置
        val filtered = text.filter { it.isDigit() }.take(5)
        uiState = uiState.copy(controlPortText = filtered)
    }

    fun onVolumeChanged(value: Int) {
        uiState = uiState.copy(volume = value.coerceIn(0, 255))
    }

    fun onDebugToneChanged(value: Boolean) {
        uiState = uiState.copy(debugTone = value)
    }

    // ------------------------------------------------------------------
    // 采集选项（变更即时生效 + 即时落盘，录音中也可改）
    // ------------------------------------------------------------------

    /** 音源模式变更（id 见 [AudioSourceIds]；UI 已对不支持的档位禁用）。 */
    fun onAudioSourceChanged(id: Int) {
        updateCapture(uiState.capture.copy(audioSource = id))
    }

    /** 输入设备变更（[CaptureSettings.MIC_DEVICE_AUTO] = 跟随系统）。 */
    fun onMicDeviceChanged(id: Int) {
        updateCapture(uiState.capture.copy(micDeviceId = id))
    }

    /** 系统降噪（NoiseSuppressor）开关。 */
    fun onNoiseSuppressorChanged(value: Boolean) {
        updateCapture(uiState.capture.copy(noiseSuppressor = value))
    }

    /** 系统回声消除（AcousticEchoCanceler）开关。 */
    fun onEchoCancelerChanged(value: Boolean) {
        updateCapture(uiState.capture.copy(echoCanceler = value))
    }

    /** 系统自动增益（AutomaticGainControl）开关。 */
    fun onGainControlChanged(value: Boolean) {
        updateCapture(uiState.capture.copy(gainControl = value))
    }

    /**
     * 统一的采集选项变更入口：改状态（重组 UI）-> 落盘（重启后保持）-> 标记待推送
     * （服务在推流时由 [pushCaptureSettings] 即时同步过去，重建 AudioRecord）。
     */
    private fun updateCapture(capture: CaptureSettings) {
        if (capture == uiState.capture) {
            return
        }
        uiState = uiState.copy(capture = capture)
        configStore?.saveCaptureSettings(capture)
        capturePushPending = true
    }

    /**
     * 把待推送的采集选项同步给运行中的服务（Activity 的 500ms 刷新循环调用）。
     * 服务未绑定/未启动时跳过：选项已落盘，下次 START 经 Intent 带入。
     */
    fun pushCaptureSettings(service: MicCaptureService?) {
        if (!capturePushPending) return
        val svc = service ?: return
        svc.updateCaptureSettings(uiState.capture)
        capturePushPending = false
    }

    // ------------------------------------------------------------------
    // 主题（变更即时生效 + 即时落盘）
    // ------------------------------------------------------------------

    fun onThemeModeChanged(mode: ThemeMode) {
        updateTheme(themeState.copy(mode = mode))
    }

    fun onThemePaletteChanged(palette: AppPalette) {
        updateTheme(themeState.copy(palette = palette))
    }

    fun onThemeAmoledChanged(value: Boolean) {
        updateTheme(themeState.copy(amoled = value))
    }

    /**
     * 统一的主题变更入口：先改状态（触发 WomicTheme 重组，即时生效无闪烁），
     * 再落盘（重启后保持）。两条验收标准（#1/#3）都靠这个方法一次满足。
     */
    private fun updateTheme(state: ThemeState) {
        themeState = state
        themeStore?.saveTheme(state)
    }

    /** 服务状态回调（主线程）。 */
    fun onServiceState(state: String, detail: String) {
        uiState = uiState.copy(state = state, detail = detail)
    }

    /** 服务监听地址变化（启动成功后刷新）。 */
    fun onListeningAddress(address: String) {
        uiState = uiState.copy(listeningAddress = address)
    }

    /** 电平回调（主线程，约 20 次/秒）。 */
    fun onLevel(rmsDb: Float, peakDb: Float) {
        uiState = uiState.copy(rmsDb = rmsDb, peakDb = peakDb)
    }

    /** 轮询统计（Activity 的 500ms 刷新循环调用）。 */
    fun pollStats(service: MicCaptureService?) {
        val svc = service ?: return
        uiState = uiState.copy(
            packets = svc.packetsSent(),
            bytes = svc.bytesSent(),
            failures = svc.sendFailures(),
            routedDeviceLabel = svc.routedDeviceLabel()
        )
    }

    /** 错误（服务会自行停止；UI 弹一次性对话框，见 [consumeMessage]）。 */
    fun onError(message: String) {
        uiState = uiState.copy(error = message)
    }

    /** 非致命提示（服务继续运行时的明确告知，如设备切换失败）。 */
    fun onMessage(message: String) {
        uiState = uiState.copy(message = message)
    }

    /** 消费一次性提示（错误优先展示；两者一起清掉）。 */
    fun consumeMessage() {
        uiState = uiState.copy(error = null, message = null)
    }
}
