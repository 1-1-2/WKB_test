package e2;

import android.net.wifi.WifiManager;
import java.io.IOException;
import java.net.ServerSocket;

/* loaded from: classes.dex */
public final class j implements e {

    /* renamed from: a, reason: collision with root package name */
    public int f12357a;

    /* renamed from: b, reason: collision with root package name */
    public ServerSocket f12358b;
    public WifiManager c;

    /* renamed from: d, reason: collision with root package name */
    public WifiManager.WifiLock f12359d;

    @Override // e2.e
    public final void a() {
        WifiManager wifiManager = this.c;
        if (!wifiManager.isWifiEnabled()) {
            throw new i();
        }
        this.f12358b = new ServerSocket(this.f12357a);
        WifiManager.WifiLock createWifiLock = wifiManager.createWifiLock(3, "WO Mic");
        this.f12359d = createWifiLock;
        createWifiLock.acquire();
    }

    @Override // e2.e
    public final f b() {
        return new f(this, this.f12358b.accept(), 2);
    }

    @Override // e2.e
    public final void close() {
        if (this.f12359d.isHeld()) {
            this.f12359d.release();
        }
        try {
            this.f12358b.close();
        } catch (IOException unused) {
        }
    }
}
