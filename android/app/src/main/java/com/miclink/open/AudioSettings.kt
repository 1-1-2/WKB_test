package com.miclink.open

import androidx.annotation.StringRes
import com.miclink.open.R

/**
 * AudioSourceIds -- 音源模式 id。
 *
 * 取值与 `android.media.MediaRecorder.AudioSource.*` 一一对应（API 冻结值）：
 * `AudioRecord.Builder.setAudioSource(int)` 收的就是这些 int，因此本模块不引用
 * framework 类也能在纯 JVM 单元测试里断言取值。对应关系：
 *
 * | 常量 | 值 | MediaRecorder.AudioSource |
 * |---|---|---|
 * | DEFAULT | 0 | DEFAULT |
 * | MIC | 1 | MIC |
 * | CAMCORDER | 5 | CAMCORDER |
 * | VOICE_RECOGNITION | 6 | VOICE_RECOGNITION |
 * | VOICE_COMMUNICATION | 7 | VOICE_COMMUNICATION |
 * | UNPROCESSED | 9 | UNPROCESSED |
 */
object AudioSourceIds {
    const val DEFAULT = 0
    const val MIC = 1
    const val CAMCORDER = 5
    const val VOICE_RECOGNITION = 6
    const val VOICE_COMMUNICATION = 7
    const val UNPROCESSED = 9
}

/**
 * 音源选项目录（UI 单选列表 + 合法性校验表共用同一份，避免两处漂移）。
 *
 * @param id 音源 id（见 [AudioSourceIds]）。
 * @param labelRes 选项名。
 * @param descRes 选项说明（实际差异直接影响用户选择，必须写清）。
 */
enum class AudioSourceOption(
    val id: Int,
    @param:StringRes val labelRes: Int,
    @param:StringRes val descRes: Int
) {
    DEFAULT(
        AudioSourceIds.DEFAULT,
        R.string.capture_source_default,
        R.string.capture_source_default_desc
    ),
    MIC(
        AudioSourceIds.MIC,
        R.string.capture_source_mic,
        R.string.capture_source_mic_desc
    ),
    VOICE_RECOGNITION(
        AudioSourceIds.VOICE_RECOGNITION,
        R.string.capture_source_voice_recognition,
        R.string.capture_source_voice_recognition_desc
    ),
    VOICE_COMMUNICATION(
        AudioSourceIds.VOICE_COMMUNICATION,
        R.string.capture_source_voice_communication,
        R.string.capture_source_voice_communication_desc
    ),
    CAMCORDER(
        AudioSourceIds.CAMCORDER,
        R.string.capture_source_camcorder,
        R.string.capture_source_camcorder_desc
    ),
    UNPROCESSED(
        AudioSourceIds.UNPROCESSED,
        R.string.capture_source_unprocessed,
        R.string.capture_source_unprocessed_desc
    );

    companion object {
        /** 按音源 id 查选项（未知 id 返回 null，由 [CaptureSettings.sanitize] 回落默认档）。 */
        fun byId(id: Int): AudioSourceOption? = entries.firstOrNull { it.id == id }
    }
}

/**
 * 采集选项快照（持久化于 Prefs，随 START Intent 送入服务）。
 *
 * **默认值 = 改动前的行为**（硬约束）：
 *  - 音源 [AudioSourceIds.DEFAULT]（改动前 openRecorder 硬编码 DEFAULT）；
 *  - 麦克风 [MIC_DEVICE_AUTO]（改动前不调用 setPreferredDevice，跟随系统）；
 *  - 三个系统效果全部 false（改动前不附加任何 NoiseSuppressor/AEC/AGC）。
 */
data class CaptureSettings(
    val audioSource: Int = AudioSourceIds.DEFAULT,
    val micDeviceId: Int = MIC_DEVICE_AUTO,
    val noiseSuppressor: Boolean = false,
    val echoCanceler: Boolean = false,
    val gainControl: Boolean = false
) {
    /** 是否有任何一个系统效果开关打开（false 时不必创建效果对象）。 */
    fun hasAnyEffect(): Boolean = noiseSuppressor || echoCanceler || gainControl

    /**
     * 清洗非法/不可用取值（纯函数，便于单测；Prefs 读取与服务接收 Intent 时都会过一遍）：
     *  - 未知音源 id -> DEFAULT；
     *  - 设备不支持 UNPROCESSED 时 -> DEFAULT（不能选了没效果，见系统属性
     *    `AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED`）；
     *  - 设备 id 只接受 -1（自动）或 >= 0。
     */
    fun sanitize(unprocessedSupported: Boolean): CaptureSettings {
        var result = this
        if (AudioSourceOption.byId(result.audioSource) == null) {
            result = result.copy(audioSource = AudioSourceIds.DEFAULT)
        }
        if (result.audioSource == AudioSourceIds.UNPROCESSED && !unprocessedSupported) {
            result = result.copy(audioSource = AudioSourceIds.DEFAULT)
        }
        if (result.micDeviceId < MIC_DEVICE_AUTO) {
            result = result.copy(micDeviceId = MIC_DEVICE_AUTO)
        }
        return result
    }

    companion object {
        /** 跟随系统：不调用 setPreferredDevice，由系统自动路由输入设备。 */
        const val MIC_DEVICE_AUTO = -1
    }
}

/** 输入设备选项（UI 列表项，id 即 [android.media.AudioDeviceInfo.getId]）。 */
data class AudioDeviceOption(
    val id: Int,
    val label: String,
    val type: Int
)

/**
 * 输入设备类型 -> 中文名。
 *
 * 引用的 `AudioDeviceInfo.TYPE_*` 均为编译期常量（static final int），
 * 会被内联进字节码，因此本文件在纯 JVM 单元测试里也可安全调用。
 *
 * @param productName 设备自报名称（如 "USB Audio Device"），非空时附在类型名后，
 *        便于区分同类型设备；为空时只显示类型名。
 */
fun inputDeviceLabel(type: Int, productName: String?): String {
    val base = when (type) {
        android.media.AudioDeviceInfo.TYPE_BUILTIN_MIC -> "内置麦克风"
        android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET -> "有线耳机麦克风"
        android.media.AudioDeviceInfo.TYPE_USB_DEVICE -> "USB 音频设备"
        android.media.AudioDeviceInfo.TYPE_USB_HEADSET -> "USB 耳机"
        android.media.AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "蓝牙耳机麦克风"
        android.media.AudioDeviceInfo.TYPE_TELEPHONY -> "通话麦克风"
        android.media.AudioDeviceInfo.TYPE_AUX_LINE -> "线路输入"
        android.media.AudioDeviceInfo.TYPE_HDMI -> "HDMI 音频设备"
        android.media.AudioDeviceInfo.TYPE_HEARING_AID -> "助听器"
        android.media.AudioDeviceInfo.TYPE_FM_TUNER -> "FM 调谐器"
        android.media.AudioDeviceInfo.TYPE_REMOTE_SUBMIX -> "远程混音（录屏等）"
        android.media.AudioDeviceInfo.TYPE_DOCK -> "Dock 音频设备"
        android.media.AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB 配件"
        else -> "输入设备（type=$type）"
    }
    val name = productName?.takeIf { it.isNotBlank() }
    return if (name != null) "$base · $name" else base
}

/** 输入设备排序优先级：内置麦克风优先，其余按常见接入方式排列。 */
private val INPUT_TYPE_ORDER = listOf(
    android.media.AudioDeviceInfo.TYPE_BUILTIN_MIC,
    android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET,
    android.media.AudioDeviceInfo.TYPE_USB_DEVICE,
    android.media.AudioDeviceInfo.TYPE_USB_HEADSET,
    android.media.AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
    android.media.AudioDeviceInfo.TYPE_TELEPHONY,
    android.media.AudioDeviceInfo.TYPE_AUX_LINE,
    android.media.AudioDeviceInfo.TYPE_HDMI,
    android.media.AudioDeviceInfo.TYPE_HEARING_AID,
    android.media.AudioDeviceInfo.TYPE_FM_TUNER,
    android.media.AudioDeviceInfo.TYPE_REMOTE_SUBMIX,
    android.media.AudioDeviceInfo.TYPE_DOCK,
    android.media.AudioDeviceInfo.TYPE_USB_ACCESSORY
)

/** 设备列表排序（类型优先、同名按 label、再按 id 保证稳定）。 */
fun sortInputDevices(devices: List<AudioDeviceOption>): List<AudioDeviceOption> {
    val order = INPUT_TYPE_ORDER.withIndex().associate { (index, type) -> type to index }
    return devices.sortedWith(
        compareBy({ order[it.type] ?: INPUT_TYPE_ORDER.size }, { it.label }, { it.id })
    )
}

/**
 * 同名设备消歧：多颗麦克风常同名（都报"内置麦克风"），追加 id 后缀，
 * 否则用户在列表里看到两行一模一样的选项、无法分辨选了哪个。
 */
fun withDisambiguatedLabels(devices: List<AudioDeviceOption>): List<AudioDeviceOption> {
    val counts = devices.groupingBy { it.label }.eachCount()
    return devices.map { device ->
        if ((counts[device.label] ?: 1) > 1) {
            device.copy(label = "${device.label}（id ${device.id}）")
        } else {
            device
        }
    }
}
