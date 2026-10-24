package com.miclink.open

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.JUnit4

/**
 * 采集选项（CaptureSettings）的纯 JVM 单元测试（无 Android framework 依赖）。
 *
 * 覆盖任务书里**可机器验证**的部分：
 *  - 默认值 = 改动前行为（DEFAULT 音源 / 跟随系统设备 / 无系统效果）；
 *  - sanitize：未知音源回落、UNPROCESSED 无设备支持时回落、非法设备 id 收敛；
 *  - 音源目录完整性（六档、byId 可往返）；
 *  - Prefs.verifyDefaults() 启动自检（含采集选项默认值断言）。
 *
 * 界面渲染、设备枚举、setPreferredDevice 实际行为依赖 Android 框架/真机，
 * 由手工验收覆盖（沙箱无模拟器）。
 */
@RunWith(JUnit4::class)
class CaptureSettingsTest {

    @Test
    fun defaults_preservePreChangeBehavior() {
        val settings = CaptureSettings()
        // 改动前 openRecorder 硬编码 MediaRecorder.AudioSource.DEFAULT（= 0）
        assertEquals(AudioSourceIds.DEFAULT, settings.audioSource)
        // 改动前不调用 setPreferredDevice
        assertEquals(CaptureSettings.MIC_DEVICE_AUTO, settings.micDeviceId)
        // 改动前不附加任何 NoiseSuppressor/AEC/AGC
        assertFalse(settings.hasAnyEffect())
    }

    @Test
    fun sanitize_unknownSourceFallsBackToDefault() {
        val dirty = CaptureSettings(audioSource = 12345)
        assertEquals(AudioSourceIds.DEFAULT, dirty.sanitize(unprocessedSupported = true).audioSource)
        assertEquals(AudioSourceIds.DEFAULT, dirty.sanitize(unprocessedSupported = false).audioSource)
    }

    @Test
    fun sanitize_unprocessedRejectedWhenUnsupported() {
        val unprocessed = CaptureSettings(audioSource = AudioSourceIds.UNPROCESSED)
        // 设备不支持：回落 DEFAULT（不能选了没效果）
        assertEquals(
            AudioSourceIds.DEFAULT,
            unprocessed.sanitize(unprocessedSupported = false).audioSource
        )
        // 设备支持：保留
        assertEquals(
            AudioSourceIds.UNPROCESSED,
            unprocessed.sanitize(unprocessedSupported = true).audioSource
        )
    }

    @Test
    fun sanitize_invalidDeviceIdConvergesToAuto() {
        // 只接受 -1（自动）或 >= 0
        assertEquals(
            CaptureSettings.MIC_DEVICE_AUTO,
            CaptureSettings(micDeviceId = -5).sanitize(true).micDeviceId
        )
        assertEquals(
            7,
            CaptureSettings(micDeviceId = 7).sanitize(true).micDeviceId
        )
    }

    @Test
    fun sanitize_keepsEffectToggles() {
        val on = CaptureSettings(
            noiseSuppressor = true,
            echoCanceler = true,
            gainControl = true
        )
        assertTrue(on.hasAnyEffect())
        val sanitized = on.sanitize(unprocessedSupported = true)
        assertTrue(sanitized.noiseSuppressor)
        assertTrue(sanitized.echoCanceler)
        assertTrue(sanitized.gainControl)
    }

    @Test
    fun audioSourceOption_catalogCoversAllIds_roundTrips() {
        // DEFAULT / MIC / VOICE_RECOGNITION / VOICE_COMMUNICATION / CAMCORDER / UNPROCESSED
        assertEquals(6, AudioSourceOption.entries.size)
        AudioSourceOption.entries.forEach { option ->
            assertNotNull(AudioSourceOption.byId(option.id))
            assertEquals(option, AudioSourceOption.byId(option.id))
        }
        assertNull(AudioSourceOption.byId(9999))
    }

    @Test
    fun audioSourceIds_matchMediaRecorderValues() {
        // android.media.MediaRecorder.AudioSource 的 API 冻结值（框架类在纯 JVM
        // 测试里不可加载，这里内联同一组常量做交叉校验，防手滑改错）
        assertEquals(0, AudioSourceIds.DEFAULT)
        assertEquals(1, AudioSourceIds.MIC)
        assertEquals(5, AudioSourceIds.CAMCORDER)
        assertEquals(6, AudioSourceIds.VOICE_RECOGNITION)
        assertEquals(7, AudioSourceIds.VOICE_COMMUNICATION)
        assertEquals(9, AudioSourceIds.UNPROCESSED)
    }

    @Test
    fun prefsVerifyDefaults_includesCaptureDefaults() {
        // 与 MainActivity 启动自检同源：默认值链（BuildConfig -> Prefs ->
        // CaptureSettings -> ThemePrefs）任何一环被改坏，不启动 App 就能抓到
        Prefs.verifyDefaults()
    }
}
