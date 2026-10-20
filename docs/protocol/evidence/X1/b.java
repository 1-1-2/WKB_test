package X1;

import android.media.AudioRecord;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.NoiseSuppressor;
import android.util.Log;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

/* loaded from: classes.dex */
public final class b {

    /* renamed from: a, reason: collision with root package name */
    public AudioRecord f1189a;

    /* renamed from: b, reason: collision with root package name */
    public int f1190b;
    public int c;

    /* renamed from: d, reason: collision with root package name */
    public int f1191d;

    /* renamed from: e, reason: collision with root package name */
    public int f1192e;
    public ArrayBlockingQueue f;

    /* renamed from: g, reason: collision with root package name */
    public int f1193g;

    /* renamed from: h, reason: collision with root package name */
    public Thread f1194h;

    /* renamed from: i, reason: collision with root package name */
    public AcousticEchoCanceler f1195i;

    /* renamed from: j, reason: collision with root package name */
    public NoiseSuppressor f1196j;

    public final void a() {
        if (this.f1193g != 1) {
            return;
        }
        this.f1193g = 0;
        try {
            this.f1194h.join();
        } catch (InterruptedException unused) {
        }
        this.f1189a.stop();
        this.f1189a.release();
        this.f1189a = null;
        AcousticEchoCanceler acousticEchoCanceler = this.f1195i;
        if (acousticEchoCanceler != null) {
            acousticEchoCanceler.release();
            this.f1195i = null;
        }
        NoiseSuppressor noiseSuppressor = this.f1196j;
        if (noiseSuppressor != null) {
            noiseSuppressor.release();
            this.f1196j = null;
        }
        this.f.clear();
    }

    public final a b() {
        int i3;
        a aVar = null;
        while (true) {
            i3 = this.f1193g;
            if (i3 != 1 || aVar != null) {
                break;
            }
            try {
                aVar = (a) this.f.poll(20L, TimeUnit.MILLISECONDS);
            } catch (InterruptedException unused) {
            }
        }
        if (i3 == 1) {
            return aVar;
        }
        throw new IllegalStateException();
    }

    public final boolean c(int i3) {
        int minBufferSize = AudioRecord.getMinBufferSize(i3, 16, 2);
        int i4 = ((this.c * i3) / 1000) * 10;
        try {
            AudioRecord audioRecord = new AudioRecord(this.f1190b, i3, 16, 2, minBufferSize < i4 ? i4 : minBufferSize);
            this.f1189a = audioRecord;
            if (audioRecord.getState() != 1) {
                this.f1189a.release();
                this.f1189a = null;
                return false;
            }
            int audioSessionId = this.f1189a.getAudioSessionId();
            if (AcousticEchoCanceler.isAvailable()) {
                AcousticEchoCanceler create = AcousticEchoCanceler.create(audioSessionId);
                this.f1195i = create;
                if (create != null && !create.getEnabled()) {
                    this.f1195i.setEnabled(true);
                }
            }
            if (NoiseSuppressor.isAvailable()) {
                NoiseSuppressor create2 = NoiseSuppressor.create(audioSessionId);
                this.f1196j = create2;
                if (create2 != null && !create2.getEnabled()) {
                    this.f1196j.setEnabled(true);
                }
            }
            return true;
        } catch (IllegalArgumentException e3) {
            Log.e("MicAudioCapture", e3.toString());
            return false;
        }
    }
}
