package V1;

import android.net.Uri;
import android.util.Log;
import b2.AbstractC0175c;
import b2.C0173a;
import b2.C0174b;
import b2.C0176d;
import c2.C0209a;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Map;
import java.util.concurrent.LinkedBlockingQueue;

/* loaded from: classes.dex */
public final class l extends Thread {

    /* renamed from: e, reason: collision with root package name */
    public final /* synthetic */ int f1136e;
    public final /* synthetic */ Object f;

    public /* synthetic */ l(Object obj, int i3) {
        this.f1136e = i3;
        this.f = obj;
    }

    /* JADX WARN: Multi-variable type inference failed */
    /* JADX WARN: Type inference failed for: r1v11, types: [d2.e] */
    /* JADX WARN: Type inference failed for: r1v13, types: [d2.e] */
    /* JADX WARN: Type inference failed for: r1v14, types: [java.lang.Object] */
    /* JADX WARN: Type inference failed for: r1v15, types: [b2.b] */
    /* JADX WARN: Type inference failed for: r1v2, types: [b2.b] */
    /* JADX WARN: Type inference failed for: r1v3, types: [b2.c] */
    /* JADX WARN: Type inference failed for: r1v4, types: [d2.b, java.lang.Object] */
    /* JADX WARN: Type inference failed for: r1v5, types: [d2.e] */
    /* JADX WARN: Type inference failed for: r1v7, types: [d2.e] */
    /* JADX WARN: Type inference failed for: r1v8, types: [d2.e] */
    /* JADX WARN: Type inference failed for: r1v9, types: [d2.e] */
    public void a(e2.f fVar) {
        AbstractC0175c abstractC0175c;
        ?? c0174b;
        InputStream b3 = fVar.b();
        OutputStream d3 = fVar.d();
        while (true) {
            n nVar = (n) this.f;
            if (nVar.f1138a != 3) {
                return;
            }
            int read = b3.read();
            if (read == 1) {
                abstractC0175c = new C0209a(1);
            } else if (read == 2) {
                abstractC0175c = new C0209a(2);
            } else if (read == 3) {
                abstractC0175c = new C0209a(3);
            } else if (read != 10) {
                switch (read) {
                    case 101:
                        abstractC0175c = new d2.a();
                        break;
                    case 102:
                        abstractC0175c = new d2.d();
                        break;
                    case 103:
                        abstractC0175c = new d2.f();
                        break;
                    case 104:
                        abstractC0175c = new d2.g();
                        break;
                    case 105:
                        abstractC0175c = new d2.c();
                        break;
                    default:
                        C0176d c0176d = new C0176d();
                        c0176d.f2513q = read;
                        abstractC0175c = c0176d;
                        break;
                }
            } else {
                abstractC0175c = new C0209a(0);
            }
            abstractC0175c.c0(b3);
            int b02 = abstractC0175c.b0();
            if (b02 != 0) {
                switch (b02) {
                    case 101:
                        int i3 = ((d2.a) abstractC0175c).f12085q;
                        nVar.f1149n = i3;
                        int i4 = i3 >= 2 ? 0 : 10;
                        c0174b = new d2.b();
                        c0174b.f12087q = i4;
                        c0174b.f12088r = 4;
                        break;
                    case 102:
                        d2.d dVar = (d2.d) abstractC0175c;
                        int i5 = dVar.f12089q;
                        if (i5 != 2 && i5 != 100) {
                            c0174b = new d2.e(12, 0);
                            break;
                        } else {
                            nVar.f1150o = i5;
                            int i6 = dVar.f12090r;
                            try {
                                int[] iArr = {8000, 16000, 48000};
                                if (i6 >= 0 && i6 < 3) {
                                    nVar.f1151p = iArr[i6];
                                    nVar.f1152q = dVar.f12091s;
                                    c0174b = new d2.e(0, 0);
                                    break;
                                } else {
                                    throw new IllegalArgumentException("Invalid index: " + i6);
                                    break;
                                }
                            } catch (IllegalArgumentException unused) {
                                c0174b = new d2.e(13, 0);
                                break;
                            }
                        }
                    case 103:
                        try {
                            nVar.f1153r.h(10, 0, nVar.f1144i.c(nVar.f1152q));
                            c0174b = new d2.e(0, 1);
                            break;
                        } catch (IOException unused2) {
                            c0174b = new d2.e(14, 1);
                            break;
                        }
                    case 104:
                        nVar.f1153r.g(11);
                        c0174b = new d2.e();
                        c0174b.f12093r = 0;
                        break;
                    case 105:
                        nVar.f1142g = true;
                        c0174b = new C0173a();
                        break;
                    default:
                        c0174b = new C0174b("");
                        break;
                }
            } else {
                c0174b = new C0174b("Unknown req: " + ((C0176d) abstractC0175c).f2513q);
            }
            c0174b.d0(d3);
        }
    }

    @Override // java.lang.Thread, java.lang.Runnable
    public final void run() {
        switch (this.f1136e) {
            case 0:
                n nVar = (n) this.f;
                try {
                    a(nVar.f1144i);
                    return;
                } catch (IOException unused) {
                    nVar.f1153r.g(30);
                    return;
                }
            case 1:
                Uri.Builder buildUpon = Uri.parse("https://pagead2.googlesyndication.com/pagead/gen_204?id=gmob-apps").buildUpon();
                Map map = (Map) this.f;
                for (String str : map.keySet()) {
                    buildUpon.appendQueryParameter(str, (String) map.get(str));
                }
                String uri = buildUpon.build().toString();
                try {
                    HttpURLConnection httpURLConnection = (HttpURLConnection) new URL(uri).openConnection();
                    try {
                        int responseCode = httpURLConnection.getResponseCode();
                        if (responseCode < 200 || responseCode >= 300) {
                            StringBuilder sb = new StringBuilder(String.valueOf(uri).length() + 65);
                            sb.append("Received non-success response code ");
                            sb.append(responseCode);
                            sb.append(" from pinging URL: ");
                            sb.append(uri);
                            Log.w("HttpUrlPinger", sb.toString());
                        }
                        httpURLConnection.disconnect();
                        return;
                    } catch (Throwable th) {
                        httpURLConnection.disconnect();
                        throw th;
                    }
                } catch (IOException e3) {
                    e = e3;
                    String message = e.getMessage();
                    StringBuilder sb2 = new StringBuilder(String.valueOf(message).length() + String.valueOf(uri).length() + 27);
                    sb2.append("Error while pinging URL: ");
                    sb2.append(uri);
                    sb2.append(". ");
                    sb2.append(message);
                    Log.w("HttpUrlPinger", sb2.toString(), e);
                    return;
                } catch (IndexOutOfBoundsException e4) {
                    String message2 = e4.getMessage();
                    StringBuilder sb3 = new StringBuilder(String.valueOf(message2).length() + String.valueOf(uri).length() + 32);
                    sb3.append("Error while parsing ping URL: ");
                    sb3.append(uri);
                    sb3.append(". ");
                    sb3.append(message2);
                    Log.w("HttpUrlPinger", sb3.toString(), e4);
                    return;
                } catch (RuntimeException e5) {
                    e = e5;
                    String message3 = e.getMessage();
                    StringBuilder sb22 = new StringBuilder(String.valueOf(message3).length() + String.valueOf(uri).length() + 27);
                    sb22.append("Error while pinging URL: ");
                    sb22.append(uri);
                    sb22.append(". ");
                    sb22.append(message3);
                    Log.w("HttpUrlPinger", sb22.toString(), e);
                    return;
                } finally {
                }
            case 2:
                E1.e eVar = (E1.e) this.f;
                boolean z3 = false;
                while (!z3) {
                    try {
                        Z1.a aVar = (Z1.a) ((LinkedBlockingQueue) eVar.f).take();
                        int i3 = aVar.f1412a;
                        if (i3 == -1) {
                            z3 = true;
                        } else {
                            ((A.g) eVar.f331g).w(i3, aVar.f1413b);
                        }
                    } catch (InterruptedException unused2) {
                        Log.e("MEssageLooper", "InterruptedException!");
                    }
                }
                return;
            default:
                new d1.i(null).g((String) this.f);
                return;
        }
    }
}
