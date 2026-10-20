package d2;

import b2.AbstractC0175c;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;

/* loaded from: classes.dex */
public final class a extends AbstractC0175c {

    /* renamed from: q, reason: collision with root package name */
    public int f12085q;

    /* renamed from: r, reason: collision with root package name */
    public int f12086r;

    @Override // b2.AbstractC0175c
    public final int b0() {
        return 101;
    }

    @Override // b2.AbstractC0175c
    public final void c0(InputStream inputStream) {
        DataInputStream dataInputStream = new DataInputStream(inputStream);
        int readInt = dataInputStream.readInt();
        this.f12086r = readInt;
        if (readInt < 6) {
            throw new IOException("Invalid request - too short.");
        }
        this.f12085q = dataInputStream.readByte();
        dataInputStream.readByte();
        dataInputStream.readByte();
        dataInputStream.readByte();
        dataInputStream.readByte();
        dataInputStream.readByte();
        dataInputStream.skipBytes(this.f12086r - 6);
    }
}
