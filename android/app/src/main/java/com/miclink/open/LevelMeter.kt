package com.miclink.open

import kotlin.math.log10
import kotlin.math.sqrt

/**
 * LevelMeter -- PCM 电平计算（RMS / peak），供 UI 电平条与"有没有声音"判断使用。
 *
 * 约定：
 *   - 输入统一为 16-bit PCM（ShortArray，小端、有符号），单声道按 [frames] 个样本处理；
 *   - dBFS 参考满量程 = 32768；静音返回 -120 dBFS；
 *   - [dbToLevel] 把 [-60, 0] dBFS 线性映射到 [0, 1]，供 ProgressBar（0..1000）使用。
 *
 * 本类是纯函数工具，无状态、线程安全。
 */
object LevelMeter {

    /** 满量程参考值（16-bit 有符号单边的绝对值上限 + 1）。 */
    private const val FULL_SCALE = 32768.0

    /** 静音 dBFS（低于任何真实信号，用于 UI 不显示 -Infinity）。 */
    const val SILENCE_DB = -120.0f

    /** UI 电平条的动态范围下限（dBFS）。 */
    const val DB_FLOOR = -60.0f

    /** 一次电平测量的结果。 */
    data class Level(
        /** RMS 电平（dBFS）。 */
        val rmsDb: Float,
        /** 峰值电平（dBFS）。 */
        val peakDb: Float,
        /** RMS 映射到 0..1（供电平条使用）。 */
        val rmsLevel: Float,
        /** 峰值映射到 0..1。 */
        val peakLevel: Float
    )

    /**
     * 计算一帧 PCM 的 RMS 与峰值。
     *
     * @param pcm    16-bit PCM 样本
     * @param offset 起始下标
     * @param frames 参与计算的样本数（单声道即帧数）
     * @return [Level]；frames <= 0 时返回静音
     */
    fun analyze(pcm: ShortArray, offset: Int = 0, frames: Int = pcm.size): Level {
        if (frames <= 0 || offset < 0 || offset >= pcm.size) {
            return Level(SILENCE_DB, SILENCE_DB, 0.0f, 0.0f)
        }
        val count = minOf(frames, pcm.size - offset)
        if (count <= 0) {
            return Level(SILENCE_DB, SILENCE_DB, 0.0f, 0.0f)
        }

        var sumSquares = 0.0
        var peak = 0
        var i = offset
        val end = offset + count
        while (i < end) {
            val s = pcm[i].toInt()
            val abs = if (s >= 0) s else -s
            if (abs > peak) {
                peak = abs
            }
            sumSquares += (s.toDouble() * s.toDouble())
            i++
        }

        val meanSquare = sumSquares / count.toDouble()
        val rms = sqrt(meanSquare)

        val rmsDb = toDb(rms)
        val peakDb = toDb(peak.toDouble())
        return Level(rmsDb, peakDb, dbToLevel(rmsDb), dbToLevel(peakDb))
    }

    /**
     * 计算立体声（交错）帧的电平：左右声道取能量平均。
     *
     * @param pcm    交错 PCM（L,R,L,R...）
     * @param frames 每声道帧数
     */
    fun analyzeStereo(pcm: ShortArray, frames: Int): Level {
        if (frames <= 0) {
            return Level(SILENCE_DB, SILENCE_DB, 0.0f, 0.0f)
        }
        var sumSquares = 0.0
        var peak = 0
        var count = 0
        var i = 0
        while (i + 1 < pcm.size && count < frames * 2) {
            val s = pcm[i].toInt()
            val abs = if (s >= 0) s else -s
            if (abs > peak) {
                peak = abs
            }
            sumSquares += (s.toDouble() * s.toDouble())
            count++
            i++
        }
        if (count == 0) {
            return Level(SILENCE_DB, SILENCE_DB, 0.0f, 0.0f)
        }
        val rms = sqrt(sumSquares / count.toDouble())
        val rmsDb = toDb(rms)
        val peakDb = toDb(peak.toDouble())
        return Level(rmsDb, peakDb, dbToLevel(rmsDb), dbToLevel(peakDb))
    }

    /** 幅度 -> dBFS。 */
    fun toDb(amplitude: Double): Float {
        if (amplitude <= 0.0) {
            return SILENCE_DB
        }
        val db = 20.0 * log10(amplitude / FULL_SCALE)
        return if (db < SILENCE_DB) SILENCE_DB else db.toFloat()
    }

    /** dBFS -> 0..1（[DB_FLOOR] 以下为 0，0 dBFS 为 1）。 */
    fun dbToLevel(db: Float): Float {
        if (db <= DB_FLOOR) {
            return 0.0f
        }
        if (db >= 0.0f) {
            return 1.0f
        }
        val level = (db - DB_FLOOR) / (0.0f - DB_FLOOR)
        return level.coerceIn(0.0f, 1.0f)
    }

    /** 把 0..1 电平转成 ProgressBar 的整数进度（0..1000）。 */
    fun levelToProgress(level: Float): Int {
        return (level.coerceIn(0.0f, 1.0f) * 1000.0f).toInt()
    }
}
