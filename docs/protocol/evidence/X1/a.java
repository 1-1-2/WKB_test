package X1;

import java.util.Arrays;

/* loaded from: classes.dex */
public final class a {

    /* renamed from: a, reason: collision with root package name */
    public final short[] f1187a;

    /* renamed from: b, reason: collision with root package name */
    public final int f1188b;
    public final int c;

    public a(int i3, int i4, short[] sArr) {
        int length = sArr.length;
        this.f1188b = i3;
        this.c = i4;
        short[] sArr2 = new short[length];
        this.f1187a = sArr2;
        System.arraycopy(sArr, 0, sArr2, 0, length);
    }

    public static short b(short s3, int i3) {
        int i4 = s3 << i3;
        if (i4 > 32767) {
            i4 = 32767;
        }
        if (i4 < -32768) {
            i4 = -32768;
        }
        return (short) i4;
    }

    /* JADX WARN: Can't fix incorrect switch cases order, some code will duplicate */
    public final void a(int i3) {
        short[] sArr = this.f1187a;
        int i4 = 0;
        switch (i3) {
            case 0:
                for (int i5 = 0; i5 < sArr.length; i5++) {
                    Arrays.fill(sArr, (short) 0);
                }
                break;
            case 1:
                while (i4 < sArr.length) {
                    sArr[i4] = (short) (sArr[i4] >> 4);
                    i4++;
                }
                break;
            case 2:
                while (i4 < sArr.length) {
                    sArr[i4] = (short) (sArr[i4] >> 3);
                    i4++;
                }
                break;
            case 3:
                while (i4 < sArr.length) {
                    sArr[i4] = (short) (sArr[i4] >> 2);
                    i4++;
                }
                break;
            case 4:
                while (i4 < sArr.length) {
                    sArr[i4] = (short) (sArr[i4] >> 1);
                    i4++;
                }
                break;
            case 6:
                while (i4 < sArr.length) {
                    sArr[i4] = b(sArr[i4], 1);
                    i4++;
                }
                break;
            case 7:
                while (i4 < sArr.length) {
                    sArr[i4] = b(sArr[i4], 2);
                    i4++;
                }
                break;
            case 8:
                while (i4 < sArr.length) {
                    sArr[i4] = b(sArr[i4], 3);
                    i4++;
                }
                break;
            case 9:
                while (i4 < sArr.length) {
                    sArr[i4] = b(sArr[i4], 4);
                    i4++;
                }
                break;
        }
    }
}
