package e2;

import java.io.IOException;
import java.net.ServerSocket;

/* loaded from: classes.dex */
public final class g implements e {

    /* renamed from: a, reason: collision with root package name */
    public final /* synthetic */ int f12353a;

    /* renamed from: b, reason: collision with root package name */
    public int f12354b;
    public ServerSocket c;

    @Override // e2.e
    public final void a() {
        switch (this.f12353a) {
            case 0:
                this.c = new ServerSocket(this.f12354b);
                break;
            default:
                this.c = new ServerSocket(this.f12354b);
                break;
        }
    }

    @Override // e2.e
    public final f b() {
        switch (this.f12353a) {
            case 0:
                return new f(this, this.c.accept(), 0);
            default:
                return new f(this, this.c.accept());
        }
    }

    @Override // e2.e
    public final void close() {
        switch (this.f12353a) {
            case 0:
                try {
                    this.c.close();
                    break;
                } catch (IOException unused) {
                    return;
                }
            default:
                try {
                    this.c.close();
                    break;
                } catch (IOException unused2) {
                    return;
                }
        }
    }
}
