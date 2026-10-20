package e2;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothServerSocket;
import java.io.IOException;
import java.util.UUID;

/* loaded from: classes.dex */
public final class c implements e {

    /* renamed from: a, reason: collision with root package name */
    public final BluetoothAdapter f12349a;

    /* renamed from: b, reason: collision with root package name */
    public BluetoothServerSocket f12350b;

    public c(BluetoothAdapter bluetoothAdapter) {
        this.f12349a = bluetoothAdapter;
    }

    @Override // e2.e
    public final void a() {
        BluetoothAdapter bluetoothAdapter = this.f12349a;
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled()) {
            throw new a();
        }
        for (int i3 = 0; i3 < 3; i3++) {
            try {
                this.f12350b = bluetoothAdapter.listenUsingRfcommWithServiceRecord("WO Mic", UUID.fromString("40F1F754-0B2F-4FCE-8B63-7C94A806A121"));
                return;
            } catch (IOException unused) {
                try {
                    Thread.sleep(10L);
                } catch (InterruptedException unused2) {
                }
            }
        }
        throw new IOException("Failed to register Bluetooth service.");
    }

    @Override // e2.e
    public final f b() {
        return new f(this, this.f12350b.accept(), 3);
    }

    @Override // e2.e
    public final void close() {
        try {
            this.f12350b.close();
        } catch (IOException unused) {
        }
    }
}
