package d2;

import b2.AbstractC0175c;
import java.io.OutputStream;

/* loaded from: classes.dex */
public final class e extends AbstractC0175c {

    /* renamed from: q, reason: collision with root package name */
    public final /* synthetic */ int f12092q;

    /* renamed from: r, reason: collision with root package name */
    public int f12093r;

    public /* synthetic */ e(int i3, int i4) {
        this.f12092q = i4;
        this.f12093r = i3;
    }

    @Override // b2.AbstractC0175c
    public final void d0(OutputStream outputStream) {
        switch (this.f12092q) {
            case 0:
                AbstractC0175c.e0(outputStream, 102, 1);
                outputStream.write(this.f12093r);
                outputStream.flush();
                break;
            case 1:
                AbstractC0175c.e0(outputStream, 103, 1);
                outputStream.write(this.f12093r);
                outputStream.flush();
                break;
            default:
                AbstractC0175c.e0(outputStream, 104, 1);
                outputStream.write(this.f12093r);
                outputStream.flush();
                break;
        }
    }
}
