package com.miclink.open

import android.content.Context
import android.content.SharedPreferences

/**
 * Prefs -- 采集参数的 SharedPreferences 持久化。
 *
 * 保存内容：目标 IP、UDP 端口、Opus 采样率、帧长、码率、是否开启带内 FEC。
 * 默认值与 Windows 服务端对齐：
 *   - 端口 5004（server/src/config.h: listen_port）
 *   - 采样率 48000（server/src/config.h: endpoint_sample_rate / expected_opus_rate）
 *   - 帧长 20ms（server/src/config.h: frame_ms）
 */
class Prefs(context: Context) {

    /** 一次完整的采集配置快照。 */
    data class Config(
        val host: String = "",
        val port: Int = DEFAULT_PORT,
        val sampleRate: Int = DEFAULT_SAMPLE_RATE,
        val frameMs: Int = DEFAULT_FRAME_MS,
        val bitrateBps: Int = DEFAULT_BITRATE_BPS,
        val fecEnabled: Boolean = true
    ) {
        /** 由采样率与帧长推导每帧样本数（Opus 要求 2.5ms 整数倍）。 */
        fun framesPerPacket(): Int = sampleRate * frameMs / 1000

        /** 参数是否自洽（端口/采样率/帧长/码率在合法区间）。 */
        fun isValid(): Boolean {
            if (host.isBlank()) return false
            if (port !in 1..65535) return false
            if (sampleRate !in SUPPORTED_RATES) return false
            if (frameMs !in SUPPORTED_FRAME_MS) return false
            if (bitrateBps < 6000 || bitrateBps > 300000) return false
            return OpusEncoder.isValidFrame(sampleRate, framesPerPacket())
        }
    }

    companion object {
        private const val FILE_NAME = "miclink_open_prefs"

        private const val KEY_HOST = "host"
        private const val KEY_PORT = "port"
        private const val KEY_SAMPLE_RATE = "sample_rate"
        private const val KEY_FRAME_MS = "frame_ms"
        private const val KEY_BITRATE = "bitrate_bps"
        private const val KEY_FEC = "fec_enabled"

        /*
         * 以下默认值的**唯一事实来源**是 app/build.gradle.kts 里的
         * buildConfigField("DEFAULT_*")（它们又对齐 server/src/config.h 的默认值）。
         * 这里只做转发，不再写第二份 5004 / 48000 / 20 / 64000 ——
         * 改一处漏一处就会"手机连不上服务端"，且极难排查（缺陷 D-11）。
         *
         * 用 val 而不是 const val：BuildConfig 是 Gradle 生成的产物，
         * 不走 Kotlin 常量折叠，但这些值仍是运行期不变的量。
         */

        /** 与服务端 listen_port 默认值一致。 */
        val DEFAULT_PORT: Int = BuildConfig.DEFAULT_UDP_PORT
        /** 与服务端 endpoint_sample_rate / expected_opus_rate 默认值一致。 */
        val DEFAULT_SAMPLE_RATE: Int = BuildConfig.DEFAULT_SAMPLE_RATE
        /** 与服务端 frame_ms 默认值一致。 */
        val DEFAULT_FRAME_MS: Int = BuildConfig.DEFAULT_FRAME_MS
        /** 64 kbps：语音在 Wi-Fi 上的稳妥档位。 */
        val DEFAULT_BITRATE_BPS: Int = BuildConfig.DEFAULT_BITRATE

        /** 下拉可选采样率（服务端支持 16000/44100/48000，Opus 只能用 16000/48000）。 */
        val SUPPORTED_RATES = listOf(16000, 48000)
        /** 下拉可选帧长（ms）。 */
        val SUPPORTED_FRAME_MS = listOf(10, 20, 40)
        /** 下拉可选码率（bps）。 */
        val SUPPORTED_BITRATES = listOf(16000, 24000, 32000, 48000, 64000, 96000, 128000)

        /**
         * 启动自检（缺陷 D-11）：默认值散落在 build.gradle.kts / Prefs / Discovery
         * 三处，曾经是各自硬编码。现在 Kotlin 侧全部转发 BuildConfig，这里再兜一层
         * 断言：一旦有人把某一处改回硬编码、或把 build.gradle.kts 的值改成非法值，
         * 应用在启动阶段就会崩，而不是"连上了却没声音"。
         *
         * 只在类加载时跑一次，开销可忽略。
         */
        fun verifyDefaults() {
            require(DEFAULT_PORT == BuildConfig.DEFAULT_UDP_PORT) {
                "Prefs.DEFAULT_PORT(${DEFAULT_PORT}) 与 BuildConfig.DEFAULT_UDP_PORT" +
                    "(${BuildConfig.DEFAULT_UDP_PORT}) 不一致"
            }
            require(DEFAULT_SAMPLE_RATE == BuildConfig.DEFAULT_SAMPLE_RATE) {
                "Prefs.DEFAULT_SAMPLE_RATE 与 BuildConfig.DEFAULT_SAMPLE_RATE 不一致"
            }
            require(DEFAULT_FRAME_MS == BuildConfig.DEFAULT_FRAME_MS) {
                "Prefs.DEFAULT_FRAME_MS 与 BuildConfig.DEFAULT_FRAME_MS 不一致"
            }
            require(DEFAULT_BITRATE_BPS == BuildConfig.DEFAULT_BITRATE) {
                "Prefs.DEFAULT_BITRATE_BPS 与 BuildConfig.DEFAULT_BITRATE 不一致"
            }
            require(DEFAULT_SAMPLE_RATE in SUPPORTED_RATES) {
                "默认采样率 $DEFAULT_SAMPLE_RATE 不在可选列表 $SUPPORTED_RATES 里"
            }
            require(DEFAULT_FRAME_MS in SUPPORTED_FRAME_MS) {
                "默认帧长 $DEFAULT_FRAME_MS 不在可选列表 $SUPPORTED_FRAME_MS 里"
            }
            require(DEFAULT_UDP_PORT_RANGE.contains(DEFAULT_PORT)) {
                "默认端口 $DEFAULT_PORT 越界"
            }
            require(DEFAULT_BITRATE_BPS in SUPPORTED_BITRATES) {
                "默认码率 $DEFAULT_BITRATE_BPS 不在可选列表 $SUPPORTED_BITRATES 里"
            }
            val frames = DEFAULT_SAMPLE_RATE * DEFAULT_FRAME_MS / 1000
            require(OpusEncoder.isValidFrame(DEFAULT_SAMPLE_RATE, frames)) {
                "默认采样率 $DEFAULT_SAMPLE_RATE 与帧长 $DEFAULT_FRAME_MS 组合非法（每帧 $frames 样本）"
            }
        }

        /** 合法端口区间。 */
        private val DEFAULT_UDP_PORT_RANGE = 1..65535
    }

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    /** 读取配置（缺项用默认值填充）。 */
    fun load(): Config {
        return Config(
            host = prefs.getString(KEY_HOST, "") ?: "",
            port = prefs.getInt(KEY_PORT, DEFAULT_PORT),
            sampleRate = prefs.getInt(KEY_SAMPLE_RATE, DEFAULT_SAMPLE_RATE)
                .let { if (it in SUPPORTED_RATES) it else DEFAULT_SAMPLE_RATE },
            frameMs = prefs.getInt(KEY_FRAME_MS, DEFAULT_FRAME_MS)
                .let { if (it in SUPPORTED_FRAME_MS) it else DEFAULT_FRAME_MS },
            bitrateBps = prefs.getInt(KEY_BITRATE, DEFAULT_BITRATE_BPS)
                .let { if (it in SUPPORTED_BITRATES) it else DEFAULT_BITRATE_BPS },
            fecEnabled = prefs.getBoolean(KEY_FEC, true)
        )
    }

    /** 写入配置。 */
    fun save(config: Config) {
        prefs.edit()
            .putString(KEY_HOST, config.host.trim())
            .putInt(KEY_PORT, config.port)
            .putInt(KEY_SAMPLE_RATE, config.sampleRate)
            .putInt(KEY_FRAME_MS, config.frameMs)
            .putInt(KEY_BITRATE, config.bitrateBps)
            .putBoolean(KEY_FEC, config.fecEnabled)
            .apply()
    }

    /** 只更新主机（发现到服务端后回填）。 */
    fun saveHost(host: String) {
        prefs.edit().putString(KEY_HOST, host.trim()).apply()
    }
}
