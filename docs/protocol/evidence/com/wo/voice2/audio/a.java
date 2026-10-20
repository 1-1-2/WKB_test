package com.wo.voice2.audio;

import X1.b;
import X1.c;
import android.os.SystemClock;
import s1.i;

/* loaded from: classes.dex */
public final class a implements Runnable {

    /* renamed from: e, reason: collision with root package name */
    public final /* synthetic */ b f11819e;

    public a(b bVar) {
        this.f11819e = bVar;
    }

    @Override // java.lang.Runnable
    public final void run() {
        b bVar = this.f11819e;
        int i3 = bVar.f1191d;
        int i4 = bVar.c;
        int i5 = (i3 * i4) / 1000;
        int i6 = bVar.f1192e;
        int i7 = (i4 * i6) / 1000;
        short[] sArr = new short[i7];
        short[] sArr2 = new short[i5];
        c iVar = i6 == i3 ? new i(22) : new MutableResampler(i6, i3);
        bVar.f1189a.startRecording();
        long elapsedRealtime = SystemClock.elapsedRealtime();
        int i8 = 0;
        loop0: while (bVar.f1193g == 1) {
            int elapsedRealtime2 = (int) (SystemClock.elapsedRealtime() - elapsedRealtime);
            int i9 = 0;
            while (i9 != i7) {
                int read = bVar.f1189a.read(sArr, i9, i7 - i9);
                if (read < 0) {
                    break loop0;
                } else {
                    i9 += read;
                }
            }
            iVar.c(sArr, i7, sArr2, i5);
            if (bVar.f.remainingCapacity() > 0) {
                try {
                    bVar.f.put(new X1.a(i8, elapsedRealtime2, sArr2));
                } catch (InterruptedException unused) {
                }
            }
            i8++;
        }
        iVar.close();
    }
}
