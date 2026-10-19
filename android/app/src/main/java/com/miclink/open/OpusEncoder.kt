package com.miclink.open

import android.util.Log

/**
 * OpusEncoder -- libopus 的 Kotlin 侧封装（JNI 实现见 app/src/main/cpp/opus_jni.cpp）。
 *
 * 使用方式：
 * ```
 * val enc = OpusEncoder()
 * enc.create(sampleRate = 48000, channels = 1, bitrateBps = 64000, frameSize = 960)
 * val n = enc.encode(pcmShorts, outBytes)   // n > 0 才发送
 * val m = enc.plc(redundantBytes)           // m > 0 表示可发一份前向冗余包
 * enc.destroy()
 * ```
 *
 * 非线程安全：一个实例只允许被采集线程持有。
 */
class OpusEncoder {

    companion object {
        private const val TAG = "OpusEncoder"

        /** 单包 payload 上限，与 Packetizer.MAX_PAYLOAD / MICLINK_PROTO_MAX_PAYLOAD 一致。 */
        const val MAX_PAYLOAD = 4096

        /** Opus 单帧样本数上限（120ms @ 48kHz）。 */
        const val MAX_FRAME = 5760

        /** native 库加载失败原因；null 表示加载成功。 */
        private var loadError: String? = null

        /** native 库（libmiclink_opus.so）是否可用。 */
        fun isLoaded(): Boolean = loadError == null

        /** 加载失败原因（用于 UI 提示"请先按 README 放入 opus 源码"）。 */
        fun loadErrorMessage(): String? = loadError

        init {
            try {
                System.loadLibrary("miclink_opus")
            } catch (t: Throwable) {
                // 常见原因：CMake 未构建成功 / opus 源码缺失 / ABI 不匹配。
                // 不向上抛，避免类初始化失败导致整个 UI 崩溃；由调用方检查 isLoaded()。
                loadError = t.message ?: t.toString()
                Log.e(TAG, "加载 libmiclink_opus.so 失败：$loadError", t)
            }
        }

        /** libopus 版本字符串；加载失败时返回可读错误而不是崩溃。 */
        fun libraryVersion(): String {
            return try {
                OpusEncoder().nativeVersion()
            } catch (t: Throwable) {
                "不可用（${t.javaClass.simpleName}: ${t.message}）"
            }
        }

        /**
         * 校验 Opus 帧长是否合法（2.5ms 的整数倍）。
         *
         * @param sampleRate 8000/12000/16000/24000/48000
         * @param frameSize  每声道样本数
         */
        fun isValidFrame(sampleRate: Int, frameSize: Int): Boolean {
            if (sampleRate !in setOf(8000, 12000, 16000, 24000, 48000)) return false
            if (frameSize <= 0 || frameSize > MAX_FRAME) return false
            return (frameSize * 400) % sampleRate == 0
        }

        /** 由采样率与帧长(ms)推导每帧样本数。 */
        fun framesOf(sampleRate: Int, frameMs: Int): Int = sampleRate * frameMs / 1000
    }

    /** native 上下文句柄；0 表示未创建。 */
    private var handle: Long = 0L

    /** 创建时的帧长（每声道样本数），编码时必须一致。 */
    var frameSize: Int = 0
        private set

    /** 创建时的采样率。 */
    var sampleRate: Int = 0
        private set

    /** 创建时的声道数。 */
    var channels: Int = 0
        private set

    val isCreated: Boolean
        get() = handle != 0L

    /**
     * 创建编码器。
     *
     * @param sampleRate  16000 或 48000（须与服务端 --opus-rate 一致或由服务端重采样消化）
     * @param channels    1（服务端当前只支持单声道）
     * @param bitrateBps  目标码率，建议 16000~96000
     * @param frameSize   每帧每声道样本数：48000Hz*20ms=960，16000Hz*20ms=320
     * @param complexity  0..10，越大音质越好、CPU 越高（默认 5）
     * @param inbandFec   是否开启带内 FEC（开启后可配合 [plc] 发冗余包）
     * @return true 成功
     */
    fun create(
        sampleRate: Int,
        channels: Int,
        bitrateBps: Int,
        frameSize: Int,
        complexity: Int = 5,
        inbandFec: Boolean = true
    ): Boolean {
        destroy()
        if (!isLoaded()) {
            Log.e(TAG, "libmiclink_opus.so 未加载：$loadError（请按 android/README.md 放入 opus 源码后重新构建）")
            return false
        }
        if (!isValidFrame(sampleRate, frameSize)) {
            Log.e(TAG, "非法参数 sampleRate=$sampleRate frameSize=$frameSize")
            return false
        }
        if (channels != 1 && channels != 2) {
            Log.e(TAG, "非法声道数 $channels")
            return false
        }
        if (bitrateBps < 500 || bitrateBps > 300000) {
            Log.e(TAG, "非法码率 $bitrateBps")
            return false
        }
        val h = try {
            nativeCreate(sampleRate, channels, bitrateBps, frameSize, complexity,
                if (inbandFec) 1 else 0)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeCreate 异常：" + t.message, t)
            0L
        }
        if (h == 0L) {
            Log.e(TAG, "opus_encoder_create 失败")
            return false
        }
        handle = h
        this.sampleRate = sampleRate
        this.channels = channels
        this.frameSize = frameSize
        return true
    }

    /** 销毁编码器，可重复调用。 */
    fun destroy() {
        val h = handle
        if (h != 0L) {
            handle = 0L
            try {
                nativeDestroy(h)
            } catch (t: Throwable) {
                Log.w(TAG, "nativeDestroy 异常：" + t.message)
            }
        }
        frameSize = 0
        sampleRate = 0
        channels = 0
    }

    /**
     * 编码一帧 PCM（单声道时 [pcm] 长度需 >= [frameSize]；立体声需 >= frameSize*2）。
     *
     * @return payload 字节数；<=0 表示失败（负值含义见 cpp/opus_jni.h）
     */
    fun encode(pcm: ShortArray, out: ByteArray): Int {
        val h = handle
        if (h == 0L) {
            return -1
        }
        if (out.size < MAX_PAYLOAD) {
            Log.e(TAG, "输出缓冲过小 ${out.size}")
            return -3
        }
        return try {
            nativeEncode(h, pcm, frameSize, out)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeEncode 异常：" + t.message, t)
            -1
        }
    }

    /**
     * 生成前向冗余包（上一帧 payload 的重发副本）。
     *
     * 注意：**这不是解码侧的 PLC**。解码侧丢包隐藏由 Windows 服务端
     * （server/src/opus_decoder.cpp）用 opus_decode(NULL) 完成；本方法只是给服务端
     * 多一份补救材料，调用方应把结果包的 flags 打上 [Packetizer.FLAG_FEC]。
     *
     * @return 冗余包字节数；0 表示还没有历史帧；<0 为错误码
     */
    fun plc(out: ByteArray): Int {
        val h = handle
        if (h == 0L) {
            return -1
        }
        if (out.size < MAX_PAYLOAD) {
            return -3
        }
        return try {
            nativePlc(h, out)
        } catch (t: Throwable) {
            Log.e(TAG, "nativePlc 异常：" + t.message, t)
            -1
        }
    }

    /** 运行时改码率（bps）。 */
    fun setBitrate(bitrateBps: Int): Boolean {
        val h = handle
        if (h == 0L) return false
        return try {
            nativeSetBitrate(h, bitrateBps) == 0
        } catch (t: Throwable) {
            Log.w(TAG, "nativeSetBitrate 异常：" + t.message)
            false
        }
    }

    /** 运行时改复杂度（0..10）。 */
    fun setComplexity(complexity: Int): Boolean {
        val h = handle
        if (h == 0L) return false
        return try {
            nativeSetComplexity(h, complexity) == 0
        } catch (t: Throwable) {
            Log.w(TAG, "nativeSetComplexity 异常：" + t.message)
            false
        }
    }

    /** 运行时开关带内 FEC。 */
    fun setInbandFec(enabled: Boolean): Boolean {
        val h = handle
        if (h == 0L) return false
        return try {
            nativeSetInbandFec(h, if (enabled) 1 else 0) == 0
        } catch (t: Throwable) {
            Log.w(TAG, "nativeSetInbandFec 异常：" + t.message)
            false
        }
    }

    /** 兜底释放：正常情况下调用方必须显式 [destroy]。 */
    protected fun finalize() {
        destroy()
    }

    private external fun nativeCreate(
        sampleRate: Int,
        channels: Int,
        bitrate: Int,
        frameSize: Int,
        complexity: Int,
        enableFec: Int
    ): Long

    private external fun nativeDestroy(handle: Long)

    private external fun nativeEncode(
        handle: Long,
        pcm: ShortArray,
        frameSize: Int,
        out: ByteArray
    ): Int

    private external fun nativePlc(handle: Long, out: ByteArray): Int

    private external fun nativeSetBitrate(handle: Long, bitrateBps: Int): Int

    private external fun nativeSetComplexity(handle: Long, complexity: Int): Int

    private external fun nativeSetInbandFec(handle: Long, enabled: Int): Int

    private external fun nativeVersion(): String
}
