package com.womic.open;

/**
 * OpusEncoder -- OpusEncoder.kt 的 Java 镜像，用于本机 JNI 冒烟测试。
 *
 * 包名 / 类名 / native 方法签名必须与 Kotlin 侧完全一致，
 * 否则 JVM 加载 libwomic_opus.so 时会抛 UnsatisfiedLinkError —— 这正是本测试要抓的问题。
 *
 * 注意：链接的是 **假的 libopus**（opus_stub_impl.cpp），
 * 因此本测试验证的是"JNI 接线是否正确"，**不是** Opus 编码质量。
 */
public class OpusEncoder {

    static {
        System.loadLibrary("womic_opus");
    }

    private native long nativeCreate(int sampleRate, int channels, int bitrate, int frameSize,
                                     int complexity, int enableFec);

    private native void nativeDestroy(long handle);

    private native int nativeEncode(long handle, short[] pcm, int frameSize, byte[] out);

    private native int nativePlc(long handle, byte[] out);

    private native int nativeSetBitrate(long handle, int bitrateBps);

    private native int nativeSetComplexity(long handle, int complexity);

    private native int nativeSetInbandFec(long handle, int enabled);

    private native String nativeVersion();

    private long handle = 0L;

    private boolean create(int sampleRate, int channels, int bitrate, int frameSize,
                           int complexity, boolean fec) {
        handle = nativeCreate(sampleRate, channels, bitrate, frameSize, complexity, fec ? 1 : 0);
        return handle != 0L;
    }

    private void destroy() {
        if (handle != 0L) {
            nativeDestroy(handle);
            handle = 0L;
        }
    }

    private static void check(boolean cond, String what) {
        if (!cond) {
            throw new AssertionError("FAIL: " + what);
        }
        System.out.println("  [ok] " + what);
    }

    public static void main(String[] args) {
        final byte[] out = new byte[4096];
        final short[] pcm = new short[960];

        System.out.println("libopus(stub) = " + new OpusEncoder().nativeVersion());

        // 1) 合法参数：create / encode / plc 往返
        OpusEncoder enc = new OpusEncoder();
        check(enc.create(48000, 1, 64000, 960, 5, true), "create(48k,1ch,64kbps,960) 成功");
        int n = enc.nativeEncode(enc.handle, pcm, 960, out);
        check(n > 0, "encode 返回 payload 长度 = " + n);
        check(enc.nativePlc(enc.handle, out) == n, "plc 重发上一帧，长度与 encode 相同");

        // 2) 运行时参数调整
        check(enc.nativeSetBitrate(enc.handle, 32000) == 0, "setBitrate(32000)");
        check(enc.nativeSetComplexity(enc.handle, 7) == 0, "setComplexity(7)");
        check(enc.nativeSetInbandFec(enc.handle, 0) == 0, "setInbandFec(false)");

        // 3) 非法参数必须有明确错误码，而不是崩溃
        check(enc.nativeEncode(enc.handle, pcm, 960, new byte[4]) == -3,
                "输出缓冲过小 -> -3 (WOMIC_OPUS_ERR_BUF_SMALL)");
        check(enc.nativeSetBitrate(enc.handle, 1) == -2, "非法码率 -> -2 (WOMIC_OPUS_ERR_BAD_ARG)");
        check(enc.nativeSetComplexity(enc.handle, 99) == -2, "非法复杂度 -> -2");
        enc.destroy();

        // 4) 非法帧长（不是 2.5ms 整数倍）-> create 失败
        OpusEncoder bad = new OpusEncoder();
        check(!bad.create(48000, 1, 64000, 1000, 5, true), "create(frameSize=1000) 被拒绝");
        bad.destroy();

        // 5) 还没编码就取冗余 -> -5
        OpusEncoder fresh = new OpusEncoder();
        check(fresh.create(48000, 1, 64000, 960, 5, false), "create(48k,960,fec=false) 成功");
        check(fresh.nativePlc(fresh.handle, out) == -5,
                "无历史帧时 plc -> -5 (WOMIC_OPUS_ERR_NO_REDUNDANT)");
        fresh.destroy();

        // 6) 16 kHz 场景（服务端可重采样消化）
        OpusEncoder nb = new OpusEncoder();
        check(nb.create(16000, 1, 32000, 320, 5, true), "create(16k,320) 成功");
        check(nb.nativeEncode(nb.handle, new short[320], 320, out) > 0, "16k encode 成功");
        nb.destroy();

        System.out.println("JNI SMOKE TEST OK");
    }
}
