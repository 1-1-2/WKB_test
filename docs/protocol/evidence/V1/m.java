package V1;

import android.util.Log;
import com.wo.voice2.audio.OpusEncoder;
import java.io.IOException;
import java.util.concurrent.ArrayBlockingQueue;

/* loaded from: classes.dex */
public final class m extends Thread {

    /* renamed from: e, reason: collision with root package name */
    public final e2.d f1137e;
    public final /* synthetic */ n f;

    public m(n nVar, e2.d dVar) {
        this.f = nVar;
        this.f1137e = dVar;
    }

    @Override // java.lang.Thread, java.lang.Runnable
    public final void run() {
        X1.d dVar;
        n nVar = this.f;
        X1.b bVar = nVar.f1145j;
        int i3 = nVar.f1151p;
        int i4 = bVar.f1193g;
        e2.d dVar2 = this.f1137e;
        if (i4 == 0) {
            bVar.f1191d = i3;
            bVar.c = 20;
            if (bVar.c(i3)) {
                bVar.f1192e = bVar.f1191d;
            } else if (bVar.c(44100)) {
                bVar.f1192e = 44100;
            } else {
                Log.e("MicAudioCapture", "Failed to initialize AudioRecord");
            }
            bVar.f = new ArrayBlockingQueue(5);
            bVar.f1193g = 1;
            Thread thread = new Thread(new com.wo.voice2.audio.a(bVar));
            bVar.f1194h = thread;
            thread.setPriority(10);
            bVar.f1194h.start();
            int i5 = nVar.f1150o;
            int i6 = nVar.f1151p;
            if (i5 == 2) {
                OpusEncoder opusEncoder = new OpusEncoder();
                opusEncoder.f11817e = i6;
                opusEncoder.f = 1;
                opusEncoder.f11818g = 20;
                dVar = opusEncoder;
            } else {
                if (i5 != 100) {
                    throw new IllegalArgumentException(W.a.i(i5, "Unknown codec: "));
                }
                dVar = new s1.i(23);
            }
            if (!dVar.a()) {
                throw new AssertionError("Failed to open encoder");
            }
            int i7 = nVar.f1149n;
            s1.i iVar = i7 != 2 ? i7 != 3 ? i7 != 4 ? null : new s1.i(27) : new s1.i(26) : new s1.i(25);
            while (nVar.f1139b) {
                try {
                    try {
                        byte[] n3 = iVar.n(nVar.f1145j.b(), dVar, nVar.f1147l, nVar.f1148m);
                        dVar2.b(n3.length, n3);
                    } catch (IOException e3) {
                        if (nVar.f1139b) {
                            nVar.f1153r.g(40);
                        }
                        Log.e("VoiceManager", "IOException: " + e3.getMessage());
                        dVar.close();
                    }
                } catch (Throwable th) {
                    dVar.close();
                    nVar.f1145j.a();
                    dVar2.a();
                    throw th;
                }
            }
            dVar.close();
            nVar.f1145j.a();
            dVar2.a();
            return;
        }
        dVar2.a();
        nVar.f1153r.g(60);
    }
}
