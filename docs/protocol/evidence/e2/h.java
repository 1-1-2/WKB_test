package e2;

import java.net.DatagramPacket;
import java.net.DatagramSocket;

/* loaded from: classes.dex */
public final class h extends d {

    /* renamed from: a, reason: collision with root package name */
    public final /* synthetic */ int f12355a;

    /* renamed from: b, reason: collision with root package name */
    public DatagramSocket f12356b;

    @Override // e2.d
    public final void a() {
        switch (this.f12355a) {
            case 0:
                this.f12356b.close();
                break;
            default:
                this.f12356b.close();
                break;
        }
    }

    @Override // e2.d
    public final void b(int i3, byte[] bArr) {
        switch (this.f12355a) {
            case 0:
                this.f12356b.send(new DatagramPacket(bArr, 0, i3));
                break;
            default:
                this.f12356b.send(new DatagramPacket(bArr, 0, i3));
                break;
        }
    }
}
