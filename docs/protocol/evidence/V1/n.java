package V1;

import com.wo.voice2.VoiceService;
import java.util.Timer;

/* loaded from: classes.dex */
public final class n {

    /* renamed from: b, reason: collision with root package name */
    public boolean f1139b;
    public Thread c;

    /* renamed from: d, reason: collision with root package name */
    public Thread f1140d;

    /* renamed from: e, reason: collision with root package name */
    public m f1141e;
    public Timer f;

    /* renamed from: g, reason: collision with root package name */
    public boolean f1142g;

    /* renamed from: j, reason: collision with root package name */
    public X1.b f1145j;

    /* renamed from: k, reason: collision with root package name */
    public final A.g f1146k;

    /* renamed from: l, reason: collision with root package name */
    public boolean f1147l;

    /* renamed from: m, reason: collision with root package name */
    public int f1148m;

    /* renamed from: n, reason: collision with root package name */
    public int f1149n;

    /* renamed from: o, reason: collision with root package name */
    public int f1150o;

    /* renamed from: p, reason: collision with root package name */
    public int f1151p;

    /* renamed from: q, reason: collision with root package name */
    public int f1152q;

    /* renamed from: a, reason: collision with root package name */
    public int f1138a = 0;

    /* renamed from: i, reason: collision with root package name */
    public e2.f f1144i = null;

    /* renamed from: h, reason: collision with root package name */
    public e2.e f1143h = null;

    /* renamed from: r, reason: collision with root package name */
    public final E1.e f1153r = new E1.e(new A.g(this, 14));

    public n(A.g gVar) {
        this.f1146k = gVar;
    }

    public static void a(n nVar, int i3) {
        int i4 = nVar.f1138a;
        if (i4 < 2) {
            return;
        }
        if (i4 == 3) {
            nVar.b();
        }
        nVar.c(0, i3);
        nVar.f1143h.close();
        try {
            nVar.c.join();
        } catch (InterruptedException unused) {
        }
        nVar.f1143h = null;
    }

    public final void b() {
        if (this.f1138a != 3) {
            return;
        }
        boolean z3 = this.f1139b;
        if (z3 && z3) {
            this.f1139b = false;
            try {
                this.f1141e.join();
            } catch (InterruptedException unused) {
            }
            this.f.cancel();
        }
        c(2, 0);
        this.f1144i.a();
        try {
            this.f1140d.join();
        } catch (InterruptedException unused2) {
        }
        this.f1144i = null;
    }

    public final void c(int i3, int i4) {
        this.f1138a = i3;
        A.g gVar = this.f1146k;
        gVar.getClass();
        int i5 = VoiceService.f11811j;
        ((VoiceService) gVar.f).a(i3, i4);
    }
}
