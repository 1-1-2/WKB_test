package e2;

import android.bluetooth.BluetoothSocket;
import android.util.Log;
import java.io.Closeable;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.Socket;
import java.net.SocketException;

/* loaded from: classes.dex */
public final class f {

    /* renamed from: a, reason: collision with root package name */
    public final /* synthetic */ int f12351a;

    /* renamed from: b, reason: collision with root package name */
    public final Closeable f12352b;

    public /* synthetic */ f(e eVar, Closeable closeable, int i3) {
        this.f12351a = i3;
        this.f12352b = closeable;
    }

    public f(g gVar, Socket socket) {
        this.f12351a = 1;
        this.f12352b = socket;
        try {
            socket.setKeepAlive(true);
        } catch (SocketException unused) {
        }
    }

    public final void a() {
        switch (this.f12351a) {
            case 0:
                try {
                    ((Socket) this.f12352b).close();
                    break;
                } catch (IOException unused) {
                    return;
                }
            case 1:
                try {
                    ((Socket) this.f12352b).close();
                    break;
                } catch (IOException unused2) {
                    return;
                }
            case 2:
                try {
                    ((Socket) this.f12352b).close();
                    break;
                } catch (IOException unused3) {
                    return;
                }
            default:
                try {
                    ((BluetoothSocket) this.f12352b).close();
                    break;
                } catch (IOException unused4) {
                    return;
                }
        }
    }

    public final InputStream b() {
        switch (this.f12351a) {
            case 0:
                return ((Socket) this.f12352b).getInputStream();
            case 1:
                return ((Socket) this.f12352b).getInputStream();
            case 2:
                return ((Socket) this.f12352b).getInputStream();
            default:
                return ((BluetoothSocket) this.f12352b).getInputStream();
        }
    }

    public final d c(int i3) {
        switch (this.f12351a) {
            case 0:
                OutputStream outputStream = ((Socket) this.f12352b).getOutputStream();
                b bVar = new b(1);
                bVar.f12348b = outputStream;
                return bVar;
            case 1:
                InetAddress inetAddress = ((Socket) this.f12352b).getInetAddress();
                h hVar = new h(0);
                try {
                    DatagramSocket datagramSocket = new DatagramSocket();
                    hVar.f12356b = datagramSocket;
                    datagramSocket.connect(inetAddress, i3);
                    Log.d("WifiMediaChannel", "Server: " + inetAddress + ", port: " + i3);
                } catch (IOException e3) {
                    Log.e("WifiMediaChannel", e3.toString());
                }
                return hVar;
            case 2:
                InetAddress inetAddress2 = ((Socket) this.f12352b).getInetAddress();
                h hVar2 = new h(1);
                try {
                    DatagramSocket datagramSocket2 = new DatagramSocket();
                    hVar2.f12356b = datagramSocket2;
                    datagramSocket2.connect(inetAddress2, i3);
                } catch (IOException e4) {
                    Log.e("WifiMediaChannel", e4.toString());
                }
                return hVar2;
            default:
                OutputStream outputStream2 = ((BluetoothSocket) this.f12352b).getOutputStream();
                b bVar2 = new b(0);
                bVar2.f12348b = new DataOutputStream(outputStream2);
                return bVar2;
        }
    }

    public final OutputStream d() {
        switch (this.f12351a) {
            case 0:
                return ((Socket) this.f12352b).getOutputStream();
            case 1:
                return ((Socket) this.f12352b).getOutputStream();
            case 2:
                return ((Socket) this.f12352b).getOutputStream();
            default:
                return ((BluetoothSocket) this.f12352b).getOutputStream();
        }
    }
}
