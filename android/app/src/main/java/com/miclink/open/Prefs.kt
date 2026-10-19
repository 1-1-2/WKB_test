package com.miclink.open

import android.content.Context
import android.content.SharedPreferences
import android.media.AudioManager
import com.miclink.open.theme.ThemePrefs
import com.miclink.open.theme.ThemeState
import dev.womic.wifistream.protocol.SampleRates

/**
 * Prefs -- 手机侧运行参数的 SharedPreferences 持久化。
 *
 * WO Mic 协议下手机是 **TCP server + UDP sender**，PC 客户端连进来后经
 * CONFIG 消息协商编码器/采样率/媒体端口（规格 v1 第 2.3 节）。因此手机侧
 * 需要用户配置的只有：
 *   - 控制端口（TCP listen，官方默认 8125，证据 V1/f.java）
 *   - UDP 头音量字段（0-255，官方默认 5）
 *   - 调试音开关（无麦克风权限时用 440Hz 正弦验证链路）
 *   - 采集选项（[CaptureSettings]：音源模式 / 麦克风 / 系统效果，默认值 =
 *     改动前行为——DEFAULT 音源、跟随系统设备、不附加任何效果）
 *
 * 采样率不再作为手机侧偏好保存：PC 发什么就按什么编码（官方 app 亦然，
 * V1/l.java 对 CONFIG 只做合法性校验后全盘接受）。
 */
class Prefs(context: Context) {

    /** 一次完整的配置快照。 */
    data class Config(
        val controlPort: Int = DEFAULT_CONTROL_PORT,
        val volume: Int = DEFAULT_VOLUME,
        val debugTone: Boolean = false,
        val capture: CaptureSettings = CaptureSettings()
    ) {
        /** 参数是否自洽。 */
        fun isValid(): Boolean {
            if (controlPort !in 1..65535) return false
            if (volume !in 0..255) return false
            return true
        }
    }

    companion object {
        /**
         * SharedPreferences 文件名。
         *
         * 对 theme 包可见（internal 而非 private）：主题 4 键与运行参数共用同一个
         * 文件，[ThemePrefs.open] 需要按这个名字打开同一份存储。若改成 private，
         * ThemePrefs 只能把文件名再抄一遍 —— 那正是 D-11 教训里要消灭的
         * "同一事实两处硬编码"。
         */
        internal const val FILE_NAME = "womic_wifi_client_prefs"

        private const val KEY_CONTROL_PORT = "control_port"
        private const val KEY_VOLUME = "volume"
        private const val KEY_DEBUG_TONE = "debug_tone"

        // 采集选项（CaptureSettings）：与上面三个键同文件、独立键名。
        // 独立于 load()/save() 提供 loadCaptureSettings()/saveCaptureSettings()：
        // 采集选项是"改完即时落盘"的偏好（不依赖点启动服务），而端口/音量是
        // "启动前锁定"的编辑态——两者写盘时机不同，混在一条写路径里会把
        // "端口文本编辑到一半"的中间态也一起落盘。
        private const val KEY_AUDIO_SOURCE = "capture_audio_source"
        private const val KEY_MIC_DEVICE_ID = "capture_mic_device_id"
        private const val KEY_NOISE_SUPPRESSOR = "capture_noise_suppressor"
        private const val KEY_ECHO_CANCELER = "capture_echo_canceler"
        private const val KEY_GAIN_CONTROL = "capture_gain_control"

        /*
         * 以下默认值的**唯一事实来源**是 app/build.gradle.kts 里的
         * buildConfigField("DEFAULT_*")（它们又对齐 docs/protocol/womic-wifi-protocol-v1.md
         * 的协议默认值表）。这里只做转发，不再写第二份 8125 / 5 ——
         * 改一处漏一处就会"PC 连不上手机"，且极难排查（缺陷 D-11）。
         *
         * 用 val 而不是 const val：BuildConfig 是 Gradle 生成的产物，
         * 不走 Kotlin 常量折叠，但这些值仍是运行期不变的量。
         */

        /** TCP 控制端口，官方 app SharedPreferences "controlPort" 默认值。 */
        val DEFAULT_CONTROL_PORT: Int = BuildConfig.DEFAULT_CONTROL_PORT

        /** UDP 头音量字段，官方默认单位增益。 */
        val DEFAULT_VOLUME: Int = BuildConfig.DEFAULT_VOLUME

        /**
         * 启动自检（缺陷 D-11）：默认值散落曾导致"连上了却没声音"。
         * 现在 Kotlin 侧全部转发 BuildConfig，这里再兜一层断言：
         * 一旦有人把某一处改回硬编码、或把 build.gradle.kts 的值改成非法值，
         * 应用在启动阶段就会崩，而不是静默失败。
         *
         * 只在类加载时跑一次，开销可忽略。
         */
        fun verifyDefaults() {
            require(DEFAULT_CONTROL_PORT == BuildConfig.DEFAULT_CONTROL_PORT) {
                "Prefs.DEFAULT_CONTROL_PORT(${DEFAULT_CONTROL_PORT}) 与 " +
                    "BuildConfig.DEFAULT_CONTROL_PORT(${BuildConfig.DEFAULT_CONTROL_PORT}) 不一致"
            }
            require(DEFAULT_CONTROL_PORT in 1..65535) {
                "默认控制端口 $DEFAULT_CONTROL_PORT 越界"
            }
            require(DEFAULT_VOLUME == BuildConfig.DEFAULT_VOLUME) {
                "Prefs.DEFAULT_VOLUME(${DEFAULT_VOLUME}) 与 " +
                    "BuildConfig.DEFAULT_VOLUME(${BuildConfig.DEFAULT_VOLUME}) 不一致"
            }
            require(DEFAULT_VOLUME in 0..255) {
                "默认音量 $DEFAULT_VOLUME 越界（u8）"
            }
            // 协议采样率表必须非空且默认索引可达（:core SampleRates 与规格 v1 表 2-4 一致）
            require(SampleRates.DEFAULT_INDEX in SampleRates.TABLE.indices) {
                "默认采样率索引 ${SampleRates.DEFAULT_INDEX} 不在协议表 ${SampleRates.TABLE.toList()} 内"
            }
            // 采集选项默认值必须保持改动前行为（DEFAULT 音源 / 跟随系统设备 / 无系统效果）
            require(CaptureSettings().audioSource == AudioSourceIds.DEFAULT) {
                "CaptureSettings 默认音源必须是 AudioSourceIds.DEFAULT（改动前 openRecorder 行为）"
            }
            require(CaptureSettings().micDeviceId == CaptureSettings.MIC_DEVICE_AUTO) {
                "CaptureSettings 默认设备必须是 MIC_DEVICE_AUTO（改动前不调用 setPreferredDevice）"
            }
            require(!CaptureSettings().hasAnyEffect()) {
                "CaptureSettings 默认不得开启任何系统效果（改动前不附加 NS/AEC/AGC）"
            }
            // 主题默认值 + 预置调色板自洽（4 活泼 + 4 低饱和、id 唯一）
            ThemePrefs.verifyDefaults()
        }
    }

    private val appContext: Context = context.applicationContext

    private val prefs: SharedPreferences =
        appContext.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    /**
     * 主题存储：与运行参数**同一个文件**，字段读写全部委托 [ThemePrefs]
     * （本类不重复实现任何主题字段，避免两份读写逻辑漂移）。
     */
    private val themePrefs: ThemePrefs = ThemePrefs(prefs)

    /** 读取主题设置（缺项/脏值回落默认值）。 */
    fun loadTheme(): ThemeState = themePrefs.load()

    /** 保存主题设置。 */
    fun saveTheme(state: ThemeState) {
        themePrefs.save(state)
    }

    /** 读取配置（缺项用默认值填充）。 */
    fun load(): Config {
        return Config(
            controlPort = prefs.getInt(KEY_CONTROL_PORT, DEFAULT_CONTROL_PORT)
                .let { if (it in 1..65535) it else DEFAULT_CONTROL_PORT },
            volume = prefs.getInt(KEY_VOLUME, DEFAULT_VOLUME)
                .let { if (it in 0..255) it else DEFAULT_VOLUME },
            debugTone = prefs.getBoolean(KEY_DEBUG_TONE, false),
            capture = loadCaptureSettings()
        )
    }

    /** 写入配置。 */
    fun save(config: Config) {
        prefs.edit()
            .putInt(KEY_CONTROL_PORT, config.controlPort)
            .putInt(KEY_VOLUME, config.volume)
            .putBoolean(KEY_DEBUG_TONE, config.debugTone)
            .apply()
        saveCaptureSettings(config.capture)
    }

    /**
     * 读取采集选项（缺项/脏值回落默认；UNPROCESSED 需设备支持，不支持时回落
     * DEFAULT——不能"选了没效果"）。
     */
    fun loadCaptureSettings(): CaptureSettings {
        val raw = CaptureSettings(
            audioSource = prefs.getInt(KEY_AUDIO_SOURCE, AudioSourceIds.DEFAULT),
            micDeviceId = prefs.getInt(KEY_MIC_DEVICE_ID, CaptureSettings.MIC_DEVICE_AUTO),
            noiseSuppressor = prefs.getBoolean(KEY_NOISE_SUPPRESSOR, false),
            echoCanceler = prefs.getBoolean(KEY_ECHO_CANCELER, false),
            gainControl = prefs.getBoolean(KEY_GAIN_CONTROL, false)
        )
        return raw.sanitize(unprocessedSupported = isUnprocessedSupported())
    }

    /** 写入采集选项（界面每次变更即时调用，不依赖"启动服务"按钮）。 */
    fun saveCaptureSettings(settings: CaptureSettings) {
        prefs.edit()
            .putInt(KEY_AUDIO_SOURCE, settings.audioSource)
            .putInt(KEY_MIC_DEVICE_ID, settings.micDeviceId)
            .putBoolean(KEY_NOISE_SUPPRESSOR, settings.noiseSuppressor)
            .putBoolean(KEY_ECHO_CANCELER, settings.echoCanceler)
            .putBoolean(KEY_GAIN_CONTROL, settings.gainControl)
            .apply()
    }

    /**
     * 设备是否支持 UNPROCESSED 音源（系统属性，API 24+）。
     * 取不到 AudioManager 或属性异常时按不支持处理（保守，UI 会禁用该档）。
     */
    private fun isUnprocessedSupported(): Boolean {
        val am = appContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return false
        return try {
            am.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true"
        } catch (t: Throwable) {
            false
        }
    }
}
