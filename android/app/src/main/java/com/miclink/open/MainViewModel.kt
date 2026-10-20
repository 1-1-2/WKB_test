package com.miclink.open

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel

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
    // 错误提示（一次性事件，UI 展示后应调 [consumeError]）
    val error: String? = null
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

    /** Activity 创建时从 Prefs 灌入可编辑配置。 */
    fun loadConfig(prefs: Prefs) {
        val config = prefs.load()
        uiState = uiState.copy(
            controlPortText = config.controlPort.toString(),
            volume = config.volume,
            debugTone = config.debugTone
        )
    }

    /** 把当前 UI 配置读成 [Prefs.Config]（端口文本非法时返回 null）。 */
    fun readConfig(): Prefs.Config? {
        val port = uiState.controlPortText.trim().toIntOrNull() ?: return null
        if (port !in 1..65535) return null
        return Prefs.Config(
            controlPort = port,
            volume = uiState.volume,
            debugTone = uiState.debugTone
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
            failures = svc.sendFailures()
        )
    }

    fun onError(message: String) {
        uiState = uiState.copy(error = message)
    }

    fun consumeError() {
        uiState = uiState.copy(error = null)
    }
}
