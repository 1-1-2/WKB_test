package com.wo.voice2.audio;

import X1.d;

/* loaded from: classes.dex */
public class OpusEncoder implements d {

    /* renamed from: e, reason: collision with root package name */
    public int f11817e;
    public int f;

    /* renamed from: g, reason: collision with root package name */
    public int f11818g;

    static {
        System.loadLibrary("codec-o");
    }

    private native void deinitNative();

    private native int encodeNative(short[] sArr, int i3, int i4, byte[] bArr, int i5);

    private native boolean initNative(int i3, int i4, int i5);

    @Override // X1.d
    public final boolean a() {
        int i3;
        int i4 = this.f11817e;
        if ((i4 != 8000 && i4 != 12000 && i4 != 16000 && i4 != 24000 && i4 != 48000) || (i3 = this.f) != 1) {
            return false;
        }
        int i5 = this.f11818g;
        if (i5 != 20 && i5 != 40) {
            return false;
        }
        initNative(i4, i3, i5);
        return true;
    }

    @Override // X1.d
    public final void close() {
        deinitNative();
    }

    @Override // X1.d
    public final int d(short[] sArr, int i3, byte[] bArr) {
        if (sArr.length > 2560) {
            return 0;
        }
        return encodeNative(sArr, 0, i3, bArr, 0);
    }
}
