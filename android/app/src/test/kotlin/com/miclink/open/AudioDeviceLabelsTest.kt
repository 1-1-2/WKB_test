package com.miclink.open

import android.media.AudioDeviceInfo
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.JUnit4

/**
 * 输入设备标签 / 排序 / 消歧的纯 JVM 单元测试。
 *
 * 这部分是设备枚举里唯一不依赖 framework 实例的逻辑（AudioDeviceInfo.TYPE_*
 * 为编译期常量，测试里可直接用）；真实枚举与 setPreferredDevice 行为依赖
 * 真机/模拟器，沙箱内无法验证（见汇报的"未验证项"）。
 */
@RunWith(JUnit4::class)
class AudioDeviceLabelsTest {

    @Test
    fun builtinMic_labelWithoutProductName() {
        assertEquals("内置麦克风", inputDeviceLabel(AudioDeviceInfo.TYPE_BUILTIN_MIC, null))
        // 空白产品名不附加
        assertEquals("内置麦克风", inputDeviceLabel(AudioDeviceInfo.TYPE_BUILTIN_MIC, "  "))
    }

    @Test
    fun label_appendsProductName() {
        assertEquals(
            "USB 音频设备 · USB Audio Device",
            inputDeviceLabel(AudioDeviceInfo.TYPE_USB_DEVICE, "USB Audio Device")
        )
    }

    @Test
    fun unknownType_fallsBackToTypeId() {
        assertEquals("输入设备（type=999）", inputDeviceLabel(999, null))
    }

    @Test
    fun sort_putsBuiltinMicFirst_thenByAccessType() {
        val devices = listOf(
            AudioDeviceOption(id = 30, label = "蓝牙耳机麦克风", type = AudioDeviceInfo.TYPE_BLUETOOTH_SCO),
            AudioDeviceOption(id = 10, label = "USB 音频设备", type = AudioDeviceInfo.TYPE_USB_DEVICE),
            AudioDeviceOption(id = 1, label = "内置麦克风", type = AudioDeviceInfo.TYPE_BUILTIN_MIC)
        )
        val sorted = sortInputDevices(devices)
        assertEquals(
            listOf(AudioDeviceInfo.TYPE_BUILTIN_MIC, AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_BLUETOOTH_SCO),
            sorted.map { it.type }
        )
    }

    @Test
    fun duplicateLabels_getIdSuffix_uniqueLabelsUntouched() {
        val devices = listOf(
            AudioDeviceOption(id = 1, label = "内置麦克风", type = AudioDeviceInfo.TYPE_BUILTIN_MIC),
            AudioDeviceOption(id = 2, label = "内置麦克风", type = AudioDeviceInfo.TYPE_BUILTIN_MIC),
            AudioDeviceOption(id = 3, label = "USB 音频设备", type = AudioDeviceInfo.TYPE_USB_DEVICE)
        )
        val result = withDisambiguatedLabels(devices)
        assertEquals("内置麦克风（id 1）", result[0].label)
        assertEquals("内置麦克风（id 2）", result[1].label)
        assertEquals("USB 音频设备", result[2].label)
    }
}
