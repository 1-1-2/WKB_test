package V1;

import H1.X;
import U.w;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import androidx.preference.Preference;
import com.google.android.gms.internal.ads.Ku;
import com.wo.voice2.ui.SettingsActivity;
import java.util.ArrayList;
import java.util.Iterator;

/* loaded from: classes.dex */
public final class f {

    /* renamed from: j, reason: collision with root package name */
    public static f f1122j;

    /* renamed from: a, reason: collision with root package name */
    public boolean f1123a;

    /* renamed from: b, reason: collision with root package name */
    public int f1124b;
    public int c;

    /* renamed from: d, reason: collision with root package name */
    public int f1125d;

    /* renamed from: e, reason: collision with root package name */
    public int f1126e;
    public int f;

    /* renamed from: g, reason: collision with root package name */
    public int f1127g;

    /* renamed from: h, reason: collision with root package name */
    public final Object f1128h;

    /* renamed from: i, reason: collision with root package name */
    public final Object f1129i;

    public f() {
        this.f1128h = new Ku();
        this.f1129i = new int[256];
    }

    public f(Context context) {
        SharedPreferences sharedPreferences = context.getSharedPreferences("settings", 0);
        this.f1128h = sharedPreferences;
        this.f1124b = sharedPreferences.getInt("transport", 2);
        this.c = sharedPreferences.getInt("controlPort", 8125);
        this.f1125d = sharedPreferences.getInt("audioSource", 0);
        this.f = sharedPreferences.getInt("purchaseState", 0);
        this.f1127g = sharedPreferences.getInt("theme", 0);
        this.f1126e = sharedPreferences.getInt("volume", 5);
        this.f1123a = sharedPreferences.getBoolean("autoMute", false);
        this.f1129i = new ArrayList();
    }

    public static f a(Context context) {
        if (f1122j == null) {
            f1122j = new f(context);
        }
        return f1122j;
    }

    public void b(int i3) {
        boolean z3;
        Iterator it = ((ArrayList) this.f1129i).iterator();
        while (it.hasNext()) {
            com.wo.voice2.ui.b bVar = (com.wo.voice2.ui.b) ((e) it.next());
            bVar.getClass();
            int b3 = s.f.b(i3);
            if (b3 == 4 || b3 == 6) {
                SettingsActivity.a aVar = bVar.f11832a;
                if (aVar.f11822i0.f == 1) {
                    Preference preference = aVar.f11826n0;
                    if (preference.f2198A) {
                        preference.f2198A = false;
                        w wVar = preference.f2208K;
                        if (wVar != null) {
                            Handler handler = wVar.f1080g;
                            D0.a aVar2 = wVar.f1081h;
                            handler.removeCallbacks(aVar2);
                            handler.post(aVar2);
                        }
                    }
                } else {
                    X x3 = (X) aVar.f11823j0.f901g;
                    synchronized (x3.c) {
                        z3 = x3.f488d;
                    }
                    if ((z3 ? W.a.F(x3.f486a.f517b.getString("privacy_options_requirement_status", "UNKNOWN")) : 1) == 3) {
                        aVar.f11826n0.w("Options");
                    } else {
                        Preference preference2 = aVar.f11826n0;
                        if (preference2.f2198A) {
                            preference2.f2198A = false;
                            w wVar2 = preference2.f2208K;
                            if (wVar2 != null) {
                                Handler handler2 = wVar2.f1080g;
                                D0.a aVar3 = wVar2.f1081h;
                                handler2.removeCallbacks(aVar3);
                                handler2.post(aVar3);
                            }
                        }
                    }
                }
            }
        }
    }
}
