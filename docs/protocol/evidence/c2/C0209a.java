package c2;

import b2.AbstractC0175c;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;

/* renamed from: c2.a, reason: case insensitive filesystem */
/* loaded from: classes.dex */
public final class C0209a extends AbstractC0175c {

    /* renamed from: q, reason: collision with root package name */
    public final /* synthetic */ int f2633q;

    @Override // b2.AbstractC0175c
    public final int b0() {
        switch (this.f2633q) {
            case 0:
                return 10;
            case 1:
                return 1;
            case 2:
                return 2;
            default:
                return 3;
        }
    }

    @Override // b2.AbstractC0175c
    public final void c0(InputStream inputStream) {
        switch (this.f2633q) {
            case 0:
                DataInputStream dataInputStream = new DataInputStream(inputStream);
                dataInputStream.skipBytes(dataInputStream.readInt());
                return;
            case 1:
                DataInputStream dataInputStream2 = new DataInputStream(inputStream);
                int readInt = dataInputStream2.readInt();
                if (readInt < 2) {
                    throw new IOException("Invalid request - too short.");
                }
                dataInputStream2.readByte();
                dataInputStream2.readByte();
                dataInputStream2.skipBytes(readInt - 2);
                return;
            case 2:
                DataInputStream dataInputStream3 = new DataInputStream(inputStream);
                dataInputStream3.skipBytes(dataInputStream3.readInt());
                return;
            default:
                DataInputStream dataInputStream4 = new DataInputStream(inputStream);
                dataInputStream4.skipBytes(dataInputStream4.readInt());
                return;
        }
    }
}
