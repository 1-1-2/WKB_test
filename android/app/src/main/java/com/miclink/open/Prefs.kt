package com.miclink.open

import android.content.Context
import android.content.SharedPreferences
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
 *
 * 采样率不再作为手机侧偏好保存：PC 发什么就按什么编码（官方 app 亦然，
 * V1/l.java 对 CONFIG 只做合法性校验后全盘接受）。
 */
class Prefs(context: Context) {

    /** 一次完整的配置快照。 */
    data class Config(
        val controlPort: Int = DEFAULT_CONTROL_PORT,
        val volume: Int = DEFAULT_VOLUME,
        val debugTone: Boolean = false
    ) {
        /** 参数是否自洽。 */
        fun isValid(): Boolean {
            if (controlPort !in 1..65535) return false
            if (volume !in 0..255) return false
            return true
        }
    }

    companion object {
        private const val FILE_NAME = "womic_wifi_client_prefs"

        private const val KEY_CONTROL_PORT = "control_port"
        private const val KEY_VOLUME = "volume"
        private const val KEY_DEBUG_TONE = "debug_tone"

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
        }
    }

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    /** 读取配置（缺项用默认值填充）。 */
    fun load(): Config {
        return Config(
            controlPort = prefs.getInt(KEY_CONTROL_PORT, DEFAULT_CONTROL_PORT)
                .let { if (it in 1..65535) it else DEFAULT_CONTROL_PORT },
            volume = prefs.getInt(KEY_VOLUME, DEFAULT_VOLUME)
                .let { if (it in 0..255) it else DEFAULT_VOLUME },
            debugTone = prefs.getBoolean(KEY_DEBUG_TONE, false)
        )
    }

    /** 写入配置。 */
    fun save(config: Config) {
        prefs.edit()
            .putInt(KEY_CONTROL_PORT, config.controlPort)
            .putInt(KEY_VOLUME, config.volume)
            .putBoolean(KEY_DEBUG_TONE, config.debugTone)
            .apply()
    }
}
