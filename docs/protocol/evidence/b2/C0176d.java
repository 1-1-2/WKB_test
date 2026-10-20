package b2;

import java.io.DataInputStream;
import java.io.InputStream;

/* renamed from: b2.d, reason: case insensitive filesystem */
/* loaded from: classes.dex */
public final class C0176d extends AbstractC0175c {

    /* renamed from: q, reason: collision with root package name */
    public int f2513q;

    @Override // b2.AbstractC0175c
    public final int b0() {
        return 0;
    }

    @Override // b2.AbstractC0175c
    public final void c0(InputStream inputStream) {
        DataInputStream dataInputStream = new DataInputStream(inputStream);
        dataInputStream.skipBytes(dataInputStream.readInt());
    }
}
