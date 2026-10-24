package com.miclink.open

import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import android.util.Log

/**
 * SystemAudioEffects -- 系统音效（NS / AEC / AGC）的薄封装。
 *
 * **零自研算法红线**：这里只有 Android framework 效果类的 create/enabled/release，
 * 没有任何自研降噪/回声/增益逻辑——那些由设备 HAL 实现。
 *
 * 生命周期契约（调用方必须遵守，否则泄漏音效会话）：
 *  1. [attach] 在 AudioRecord 创建并开始录音后调用，绑定 [android.media.AudioRecord.getAudioSessionId]；
 *  2. [release] 必须在 AudioRecord.release() **之前**调用（效果持有会话，
 *     先释放录音器会让后续 release 静默失败）；
 *  3. 每次 AudioRecord 重建都要重新 attach（效果跟会话走，不跟设备走）。
 *
 * 失败语义：`isAvailable()` 为 false 或 create 返回 null 时不上抛异常，
 * 而是回调 [onFailure]（服务转成明确的用户提示）——**不假装生效**。
 */
class SystemAudioEffects {

    private var noiseSuppressor: NoiseSuppressor? = null
    private var echoCanceler: AcousticEchoCanceler? = null
    private var gainControl: AutomaticGainControl? = null

    /**
     * 按设置把打开的效果绑到录音会话上。
     *
     * @param sessionId AudioRecord 的音频会话 id。
     * @param settings 采集选项（只创建用户打开的效果）。
     * @param onFailure 单个效果启用失败时回调（中文效果名, 原因）。
     */
    fun attach(sessionId: Int, settings: CaptureSettings, onFailure: (String, String) -> Unit) {
        if (settings.noiseSuppressor) {
            if (!NoiseSuppressor.isAvailable()) {
                onFailure("降噪（NS）", "该设备不支持此效果")
            } else {
                val effect = try {
                    NoiseSuppressor.create(sessionId)
                } catch (t: Throwable) {
                    Log.w(TAG, "NoiseSuppressor.create 异常：${t.message}")
                    null
                }
                if (effect == null) {
                    onFailure("降噪（NS）", "创建失败（该设备可能不支持在此会话上启用）")
                } else {
                    effect.enabled = true
                    noiseSuppressor = effect
                    Log.i(TAG, "NoiseSuppressor 已启用（session=$sessionId）")
                }
            }
        }

        if (settings.echoCanceler) {
            if (!AcousticEchoCanceler.isAvailable()) {
                onFailure("回声消除（AEC）", "该设备不支持此效果")
            } else {
                val effect = try {
                    AcousticEchoCanceler.create(sessionId)
                } catch (t: Throwable) {
                    Log.w(TAG, "AcousticEchoCanceler.create 异常：${t.message}")
                    null
                }
                if (effect == null) {
                    onFailure("回声消除（AEC）", "创建失败（该设备可能不支持在此会话上启用）")
                } else {
                    effect.enabled = true
                    echoCanceler = effect
                    Log.i(TAG, "AcousticEchoCanceler 已启用（session=$sessionId）")
                }
            }
        }

        if (settings.gainControl) {
            if (!AutomaticGainControl.isAvailable()) {
                onFailure("自动增益（AGC）", "该设备不支持此效果")
            } else {
                val effect = try {
                    AutomaticGainControl.create(sessionId)
                } catch (t: Throwable) {
                    Log.w(TAG, "AutomaticGainControl.create 异常：${t.message}")
                    null
                }
                if (effect == null) {
                    onFailure("自动增益（AGC）", "创建失败（该设备可能不支持在此会话上启用）")
                } else {
                    effect.enabled = true
                    gainControl = effect
                    Log.i(TAG, "AutomaticGainControl 已启用（session=$sessionId）")
                }
            }
        }
    }

    /** 实际生效的效果描述（如 "NS+AGC"；全部失败时空串）。 */
    fun describe(): String {
        val parts = mutableListOf<String>()
        if (noiseSuppressor?.enabled == true) parts.add("NS")
        if (echoCanceler?.enabled == true) parts.add("AEC")
        if (gainControl?.enabled == true) parts.add("AGC")
        return parts.joinToString("+")
    }

    /** 释放全部效果（可重复调用；必须在 AudioRecord.release 之前）。 */
    fun release() {
        try {
            noiseSuppressor?.release()
        } catch (t: Throwable) {
            Log.w(TAG, "release NoiseSuppressor 异常：${t.message}")
        }
        noiseSuppressor = null
        try {
            echoCanceler?.release()
        } catch (t: Throwable) {
            Log.w(TAG, "release AcousticEchoCanceler 异常：${t.message}")
        }
        echoCanceler = null
        try {
            gainControl?.release()
        } catch (t: Throwable) {
            Log.w(TAG, "release AutomaticGainControl 异常：${t.message}")
        }
        gainControl = null
    }

    private companion object {
        const val TAG = "SystemAudioEffects"
    }
}
