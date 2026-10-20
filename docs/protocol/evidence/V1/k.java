package V1;

import java.util.TimerTask;

/* loaded from: classes.dex */
public final class k extends TimerTask {

    /* renamed from: e, reason: collision with root package name */
    public final /* synthetic */ n f1135e;

    public k(n nVar) {
        this.f1135e = nVar;
    }

    @Override // java.util.TimerTask, java.lang.Runnable
    public final void run() {
        n nVar = this.f1135e;
        if (nVar.f1142g) {
            nVar.f1142g = false;
        } else {
            nVar.f1153r.g(50);
        }
    }
}
