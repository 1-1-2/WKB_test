package e2;

import java.io.DataOutputStream;
import java.io.OutputStream;

/* loaded from: classes.dex */
public final class b extends d {

    /* renamed from: a, reason: collision with root package name */
    public final /* synthetic */ int f12347a;

    /* renamed from: b, reason: collision with root package name */
    public OutputStream f12348b;

    @Override // e2.d
    public final void b(int i3, byte[] bArr) {
        switch (this.f12347a) {
            case 0:
                DataOutputStream dataOutputStream = (DataOutputStream) this.f12348b;
                int i4 = i3;
                while (i4 > 0) {
                    int min = Math.min(i4, 40000);
                    dataOutputStream.write(bArr, i3 - i4, min);
                    i4 -= min;
                }
                dataOutputStream.flush();
                break;
            default:
                OutputStream outputStream = this.f12348b;
                int i5 = i3;
                while (i5 > 0) {
                    int min2 = Math.min(i5, 40000);
                    outputStream.write(bArr, i3 - i5, min2);
                    i5 -= min2;
                }
                outputStream.flush();
                break;
        }
    }
}
