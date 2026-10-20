package b2;

import java.io.DataOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/* renamed from: b2.c, reason: case insensitive filesystem */
/* loaded from: classes.dex */
public abstract class AbstractC0175c extends A1.b {
    public static void e0(OutputStream outputStream, int i3, int i4) {
        DataOutputStream dataOutputStream = new DataOutputStream(outputStream);
        dataOutputStream.write(i3);
        dataOutputStream.writeInt(i4);
    }

    public abstract int b0();

    public abstract void c0(InputStream inputStream);

    public abstract void d0(OutputStream outputStream);
}
