package d2;

import b2.AbstractC0175c;
import java.io.OutputStream;

/* loaded from: classes.dex */
public final class b extends AbstractC0175c {

    /* renamed from: q, reason: collision with root package name */
    public int f12087q;

    /* renamed from: r, reason: collision with root package name */
    public int f12088r;

    @Override // b2.AbstractC0175c
    public final void d0(OutputStream outputStream) {
        AbstractC0175c.e0(outputStream, 101, 2);
        outputStream.write(this.f12087q);
        outputStream.write(this.f12088r);
        outputStream.flush();
    }
}
