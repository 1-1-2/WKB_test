package d2;

import b2.AbstractC0175c;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;

/* loaded from: classes.dex */
public final class d extends AbstractC0175c {

    /* renamed from: q, reason: collision with root package name */
    public int f12089q;

    /* renamed from: r, reason: collision with root package name */
    public int f12090r;

    /* renamed from: s, reason: collision with root package name */
    public int f12091s;

    @Override // b2.AbstractC0175c
    public final int b0() {
        return 102;
    }

    @Override // b2.AbstractC0175c
    public final void c0(InputStream inputStream) {
        DataInputStream dataInputStream = new DataInputStream(inputStream);
        int readInt = dataInputStream.readInt();
        if (readInt < 6) {
            throw new IOException("Invalid request - too short.");
        }
        this.f12089q = dataInputStream.readByte();
        this.f12090r = dataInputStream.readByte();
        this.f12091s = dataInputStream.readInt();
        dataInputStream.skipBytes(readInt - 6);
    }
}
