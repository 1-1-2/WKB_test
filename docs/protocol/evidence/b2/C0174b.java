package b2;

import java.io.OutputStream;

/* renamed from: b2.b, reason: case insensitive filesystem */
/* loaded from: classes.dex */
public final class C0174b extends AbstractC0175c {

    /* renamed from: q, reason: collision with root package name */
    public final String f2512q;

    public C0174b(String str) {
        this.f2512q = str;
    }

    @Override // b2.AbstractC0175c
    public final void d0(OutputStream outputStream) {
        String str = this.f2512q;
        AbstractC0175c.e0(outputStream, 0, str != null ? str.getBytes().length + 1 : 1);
        outputStream.write(1);
        if (str != null) {
            outputStream.write(str.getBytes());
        }
    }
}
