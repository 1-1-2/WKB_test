package com.miclink.open

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * AudioDeviceManager -- 输入设备枚举 / 音源与效果能力查询（Android framework 薄封装）。
 *
 * 职责边界：
 *  - 枚举：`AudioManager.getDevices(TYPE_DEVICE_INPUTS)` -> [AudioDeviceOption]
 *    （排序与消歧是纯函数，见 [sortInputDevices] / [withDisambiguatedLabels]）；
 *  - 能力：UNPROCESSED 音源支持（`AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED`）
 *    与 NS/AEC/AGC 的 [NoiseSuppressor.isAvailable] 等静态判断——**UI 必须依据它们
 *    禁用选项**，不能假装生效；
 *  - 回调：设备热插拔通知（[registerDeviceCallback]）。
 *
 * 不持有录音状态："当前实际生效的设备"由服务的 AudioRecord.getRoutedDevice() 提供。
 */
class AudioDeviceManager(context: Context) {

    private val appContext: Context = context.applicationContext

    private val audioManager: AudioManager? =
        appContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager

    /** 可用输入设备（排序 + 消歧后）。AudioManager 不可用时返回空列表。 */
    fun inputDevices(): List<AudioDeviceOption> {
        val am = audioManager ?: return emptyList()
        val raw = try {
            am.getDevices(AudioManager.GET_DEVICES_INPUTS)
        } catch (t: Throwable) {
            Log.w(TAG, "枚举输入设备失败：${t.message}")
            emptyArray()
        }
        val options = raw.map { device ->
            AudioDeviceOption(
                id = device.id,
                label = deviceLabel(device),
                type = device.type
            )
        }
        return sortInputDevices(withDisambiguatedLabels(options))
    }

    /** 按 id 找当前输入设备（设备已拔出时返回 null）。 */
    fun deviceById(id: Int): AudioDeviceInfo? {
        if (id < 0) return null
        val am = audioManager ?: return null
        return try {
            am.getDevices(AudioManager.GET_DEVICES_INPUTS)?.firstOrNull { it.id == id }
        } catch (t: Throwable) {
            Log.w(TAG, "按 id 查找输入设备失败：${t.message}")
            null
        }
    }

    /**
     * 设备是否支持 UNPROCESSED 音源（系统属性，API 24+）。
     * 注意：属性字符串是 "true"/"false"/null，只有 "true" 才算支持。
     */
    fun isUnprocessedSupported(): Boolean {
        val am = audioManager ?: return false
        return try {
            am.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true"
        } catch (t: Throwable) {
            Log.w(TAG, "读取 UNPROCESSED 支持属性失败：${t.message}")
            false
        }
    }

    /** 系统降噪（NoiseSuppressor）是否可用（设备能力，非会话状态）。 */
    fun isNoiseSuppressorAvailable(): Boolean = NoiseSuppressor.isAvailable()

    /** 系统回声消除（AcousticEchoCanceler）是否可用。 */
    fun isEchoCancelerAvailable(): Boolean = AcousticEchoCanceler.isAvailable()

    /** 系统自动增益（AutomaticGainControl）是否可用。 */
    fun isGainControlAvailable(): Boolean = AutomaticGainControl.isAvailable()

    /** 注册设备热插拔回调（主线程）。 */
    fun registerDeviceCallback(callback: AudioDeviceCallback) {
        val am = audioManager ?: return
        try {
            am.registerAudioDeviceCallback(callback, Handler(Looper.getMainLooper()))
        } catch (t: Throwable) {
            Log.w(TAG, "注册 AudioDeviceCallback 失败：${t.message}")
        }
    }

    /** 注销设备热插拔回调（未注册时忽略）。 */
    fun unregisterDeviceCallback(callback: AudioDeviceCallback) {
        val am = audioManager ?: return
        try {
            am.unregisterAudioDeviceCallback(callback)
        } catch (t: Throwable) {
            Log.w(TAG, "注销 AudioDeviceCallback 失败：${t.message}")
        }
    }

    companion object {
        private const val TAG = "AudioDeviceManager"

        /** 单个设备的展示名（类型中文名 + 自报名）。 */
        fun deviceLabel(device: AudioDeviceInfo): String =
            inputDeviceLabel(device.type, device.productName?.toString())
    }
}
