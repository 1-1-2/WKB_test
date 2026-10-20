package d2;

import b2.AbstractC0175c;
import java.io.DataInputStream;
import java.io.InputStream;

/* loaded from: classes.dex */
public final class f extends AbstractC0175c {
    @Override // b2.AbstractC0175c
    public final int b0() {
        return 103;
    }

    @Override // b2.AbstractC0175c
    public final void c0(InputStream inputStream) {
        DataInputStream dataInputStream = new DataInputStream(inputStream);
        dataInputStream.skipBytes(dataInputStream.readInt());
    }
}
