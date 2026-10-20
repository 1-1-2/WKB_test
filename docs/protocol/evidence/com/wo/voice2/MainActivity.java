package com.wo.voice2;

import E0.v;
import H1.C0002c;
import H1.Q;
import H1.X;
import O0.k;
import O1.l;
import V1.a;
import V1.c;
import V1.d;
import V1.f;
import V1.o;
import V1.q;
import V1.r;
import W1.b;
import android.R;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.telephony.TelephonyManager;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.widget.Toast;
import androidx.fragment.app.C0114a;
import androidx.fragment.app.P;
import com.google.android.gms.internal.ads.C1631yd;
import com.wo.voice2.ui.AboutActivity;
import com.wo.voice2.ui.SettingsActivity;
import e.C1799f;
import e2.g;
import e2.j;
import java.util.ArrayList;
import java.util.concurrent.Executor;
import q0.n;
import s1.i;
import y.e;

/* loaded from: classes.dex */
public class MainActivity extends r {

    /* renamed from: C, reason: collision with root package name */
    public f f11783C;

    /* renamed from: D, reason: collision with root package name */
    public l f11784D;

    /* renamed from: E, reason: collision with root package name */
    public boolean f11785E;

    /* renamed from: F, reason: collision with root package name */
    public d f11786F;

    /* renamed from: G, reason: collision with root package name */
    public C1631yd f11787G;

    /* renamed from: J, reason: collision with root package name */
    public b f11790J;

    /* renamed from: K, reason: collision with root package name */
    public o f11791K;

    /* renamed from: L, reason: collision with root package name */
    public boolean f11792L;

    /* renamed from: M, reason: collision with root package name */
    public WifiManager f11793M;

    /* renamed from: N, reason: collision with root package name */
    public WifiManager.WifiLock f11794N;

    /* renamed from: O, reason: collision with root package name */
    public BluetoothAdapter f11795O;

    /* renamed from: P, reason: collision with root package name */
    public boolean f11796P;

    /* renamed from: Q, reason: collision with root package name */
    public boolean f11797Q;

    /* renamed from: R, reason: collision with root package name */
    public n f11798R;

    /* renamed from: H, reason: collision with root package name */
    public int f11788H = 0;

    /* renamed from: I, reason: collision with root package name */
    public boolean f11789I = false;

    /* renamed from: S, reason: collision with root package name */
    public final a f11799S = new a(this, 0);

    /* renamed from: T, reason: collision with root package name */
    public final V1.b f11800T = new V1.b(this);

    /* renamed from: U, reason: collision with root package name */
    public final c f11801U = new c(this);

    /* JADX WARN: Removed duplicated region for block: B:28:0x0139  */
    /* JADX WARN: Removed duplicated region for block: B:30:0x014c  */
    /* JADX WARN: Removed duplicated region for block: B:57:? A[RETURN, SYNTHETIC] */
    /*
        Code decompiled incorrectly, please refer to instructions dump.
        To view partially-correct add '--show-bad-code' argument
    */
    public static void E(com.wo.voice2.MainActivity r9, int r10, int r11) {
        /*
            Method dump skipped, instructions count: 475
            To view this dump add '--comments-level debug' option
        */
        throw new UnsupportedOperationException("Method not decompiled: com.wo.voice2.MainActivity.E(com.wo.voice2.MainActivity, int, int):void");
    }

    public final void F(int i3) {
        f fVar = this.f11783C;
        int i4 = fVar.f;
        SharedPreferences sharedPreferences = (SharedPreferences) fVar.f1128h;
        if (i4 == 1) {
            if (fVar.f1126e != i3) {
                fVar.f1126e = i3;
                SharedPreferences.Editor edit = sharedPreferences.edit();
                edit.putInt("volume", fVar.f1126e);
                edit.apply();
                fVar.b(4);
            }
            if (this.f11792L) {
                this.f11791K.f1154e.f11814h.f1148m = i3;
            }
            Toast.makeText(this, "New volume level: " + i3, 0).show();
            return;
        }
        if (fVar.f1126e != 5) {
            fVar.f1126e = 5;
            SharedPreferences.Editor edit2 = sharedPreferences.edit();
            edit2.putInt("volume", fVar.f1126e);
            edit2.apply();
            fVar.b(4);
        }
        if (this.f11785E) {
            return;
        }
        String string = getString(R.string.info_volume_premium_feature);
        String string2 = getString(R.string.action_name_subscribe);
        String string3 = getString(R.string.cancel);
        c cVar = new c(this);
        v vVar = new v(this);
        C1799f c1799f = (C1799f) vVar.f;
        c1799f.f12225d = "";
        c1799f.f = string;
        c1799f.f12231k = false;
        q qVar = new q(cVar, 0);
        c1799f.f12227g = string2;
        c1799f.f12228h = qVar;
        q qVar2 = new q(cVar, 1);
        c1799f.f12229i = string3;
        c1799f.f12230j = qVar2;
        vVar.e().show();
        this.f11785E = true;
    }

    /* JADX WARN: Multi-variable type inference failed */
    public final void G() {
        e2.c cVar;
        Log.d("MainActivity", "doStart()");
        if (this.f11792L) {
            ArrayList arrayList = new ArrayList();
            if (e.a(this, "android.permission.RECORD_AUDIO") != 0) {
                arrayList.add("android.permission.RECORD_AUDIO");
            }
            int i3 = Build.VERSION.SDK_INT;
            if (i3 >= 31) {
                if (this.f11783C.f1123a && e.a(this, "android.permission.READ_PHONE_STATE") != 0) {
                    arrayList.add("android.permission.READ_PHONE_STATE");
                }
                if (this.f11783C.f1124b == 0 && e.a(this, "android.permission.BLUETOOTH_CONNECT") != 0) {
                    arrayList.add("android.permission.BLUETOOTH_CONNECT");
                }
            }
            if (arrayList.size() > 0) {
                x.e.c(this, (String[]) arrayList.toArray(new String[0]), 0);
                return;
            }
            int i4 = this.f11783C.f1124b;
            if (i4 != 0) {
                if (i4 == 2 && !this.f11793M.isWifiEnabled()) {
                    this.f11797Q = true;
                    if (i3 >= 29) {
                        startActivity(new Intent("android.settings.panel.action.WIFI"));
                        return;
                    } else {
                        this.f11793M.setWifiEnabled(true);
                        return;
                    }
                }
            } else if (!this.f11795O.isEnabled()) {
                this.f11796P = true;
                startActivity(new Intent("android.bluetooth.adapter.action.REQUEST_ENABLE"));
                return;
            }
            o oVar = this.f11791K;
            f fVar = this.f11783C;
            int i5 = fVar.f1124b;
            int i6 = fVar.c;
            int i7 = fVar.f1125d;
            oVar.getClass();
            VoiceService voiceService = oVar.f1154e;
            Intent intent = new Intent(voiceService, (Class<?>) VoiceService.class);
            if (i3 >= 26) {
                y.d.b(voiceService, intent);
            } else {
                voiceService.startService(intent);
            }
            String string = voiceService.getString(R.string.notification_starting_text);
            int i8 = VoiceService.f11811j;
            voiceService.b(string, true);
            if (i5 == 0) {
                cVar = new e2.c(BluetoothAdapter.getDefaultAdapter());
            } else if (i5 == 1) {
                g gVar = new g(0);
                gVar.f12354b = i6;
                cVar = gVar;
            } else if (i5 == 2) {
                WifiManager wifiManager = (WifiManager) voiceService.getSystemService("wifi");
                j jVar = new j();
                jVar.f12357a = i6;
                jVar.c = wifiManager;
                cVar = jVar;
            } else {
                if (i5 != 3) {
                    throw new IllegalArgumentException(W.a.i(i5, "Unknown transport type: "));
                }
                g gVar2 = new g(1);
                gVar2.f12354b = i6;
                cVar = gVar2;
            }
            X1.b bVar = new X1.b();
            bVar.f1189a = null;
            bVar.f1190b = i7;
            V1.n nVar = voiceService.f11814h;
            nVar.getClass();
            ArrayList arrayList2 = new ArrayList();
            arrayList2.add(cVar);
            arrayList2.add(bVar);
            nVar.f1153r.h(0, 0, arrayList2);
            H();
        }
    }

    public final void H() {
        if (this.f11792L) {
            this.f11791K.f1154e.f11814h.f1147l = this.f11798R.f != 1;
        }
        invalidateOptionsMenu();
    }

    @Override // V1.r, e.AbstractActivityC1805l, androidx.activity.l, x.k, android.app.Activity
    public final void onCreate(Bundle bundle) {
        super.onCreate(bundle);
        setContentView(R.layout.activity_main);
        setTitle(R.string.app_name);
        if (bundle != null) {
            this.f11796P = bundle.getBoolean("state_waitingForBluetoothOn");
            this.f11797Q = bundle.getBoolean("state_waitingForWifiOn");
        } else {
            this.f11796P = false;
            this.f11797Q = false;
        }
        this.f11785E = false;
        this.f11783C = f.a(getApplicationContext());
        this.f11793M = (WifiManager) getApplicationContext().getSystemService("wifi");
        this.f11795O = ((BluetoothManager) getSystemService("bluetooth")).getAdapter();
        Context applicationContext = getApplicationContext();
        if (C1631yd.f11468k == null) {
            C1631yd c1631yd = new C1631yd(2, false);
            c1631yd.f = applicationContext;
            f.a(applicationContext);
            c1631yd.f11471h = (TelephonyManager) applicationContext.getSystemService("phone");
            C1631yd.f11468k = c1631yd;
        }
        this.f11787G = C1631yd.f11468k;
        if (n.f13303h == null) {
            n nVar = new n(3);
            nVar.f = 1;
            n.f13303h = nVar;
        }
        this.f11798R = n.f13303h;
        bindService(new Intent(getApplicationContext(), (Class<?>) VoiceService.class), this.f11800T, 1);
        if (l.f899i == null) {
            l.f899i = new l((Activity) this);
        }
        l lVar = l.f899i;
        this.f11784D = lVar;
        d dVar = new d(this, 0);
        this.f11786F = dVar;
        ((ArrayList) lVar.f901g).add(dVar);
        if (lVar.f900e) {
            dVar.a();
        }
        if (this.f11783C.f != 1) {
            if (l.f898h == null) {
                l lVar2 = new l();
                lVar2.f900e = false;
                l.f898h = lVar2;
            }
            l lVar3 = l.f898h;
            i iVar = new i(21);
            if (!lVar3.f900e) {
                lVar3.f = iVar;
                E0.c cVar = new E0.c(this);
                cVar.f264b = 1;
                ((ArrayList) cVar.f263a).add("D6B557958D3A9158325A3C24B62F1C1C");
                ((ArrayList) cVar.f263a).add("7B1751C72ABDCA1262643BA8540C02A2");
                P1.a a2 = cVar.a();
                P1.b bVar = new P1.b();
                bVar.f951a = a2;
                P1.b bVar2 = new P1.b();
                bVar2.f951a = bVar.f951a;
                X x3 = (X) ((Q) C0002c.b(this).f506k).a();
                lVar3.f901g = x3;
                k kVar = new k(lVar3, 3, this);
                K.d dVar2 = new K.d(lVar3, 6);
                synchronized (x3.c) {
                    x3.f488d = true;
                }
                C0002c c0002c = x3.f487b;
                c0002c.getClass();
                ((Executor) c0002c.f502g).execute(new A0.q(c0002c, this, bVar2, kVar, dVar2, 1, false));
            }
        }
        IntentFilter intentFilter = new IntentFilter();
        intentFilter.addAction("android.net.wifi.WIFI_STATE_CHANGED");
        intentFilter.addAction("android.bluetooth.adapter.action.STATE_CHANGED");
        intentFilter.addAction("com.wo.voice.VoiceService.ACTION_STATE");
        if (Build.VERSION.SDK_INT >= 26) {
            registerReceiver(this.f11799S, intentFilter, 4);
        } else {
            registerReceiver(this.f11799S, intentFilter);
        }
    }

    @Override // android.app.Activity
    public final boolean onCreateOptionsMenu(Menu menu) {
        getMenuInflater().inflate(R.menu.main_activity_actions, menu);
        return super.onCreateOptionsMenu(menu);
    }

    @Override // e.AbstractActivityC1805l, android.app.Activity
    public final void onDestroy() {
        if (this.f11792L) {
            unbindService(this.f11800T);
        }
        unregisterReceiver(this.f11799S);
        l lVar = this.f11784D;
        ((ArrayList) lVar.f901g).remove(this.f11786F);
        super.onDestroy();
    }

    @Override // e.AbstractActivityC1805l, android.app.Activity, android.view.KeyEvent.Callback
    public final boolean onKeyDown(int i3, KeyEvent keyEvent) {
        if (i3 == 4) {
            if (this.f11788H >= 2) {
                Toast.makeText(this, R.string.warning_service_running, 0).show();
                return true;
            }
            stopService(new Intent(this, (Class<?>) VoiceService.class));
            finish();
            System.exit(0);
        }
        return super.onKeyDown(i3, keyEvent);
    }

    @Override // android.app.Activity
    public final boolean onOptionsItemSelected(MenuItem menuItem) {
        int itemId = menuItem.getItemId();
        if (itemId == R.id.action_start) {
            if (this.f11788H == 0) {
                G();
            } else if (this.f11792L) {
                this.f11791K.f1154e.f11814h.f1153r.g(1);
            }
            return true;
        }
        if (itemId == R.id.action_mute) {
            n nVar = this.f11798R;
            int i3 = nVar.f;
            if (i3 != 1) {
                int b3 = s.f.b(i3);
                if (b3 == 1 || b3 == 2 || b3 == 3) {
                    nVar.f = 1;
                }
            } else if (s.f.b(i3) == 0) {
                nVar.f = 2;
            }
            H();
            return true;
        }
        if (itemId == R.id.action_volume_down) {
            int i4 = this.f11783C.f1126e;
            if (i4 <= 0) {
                Toast.makeText(this, "Min volume level has been reached.", 0).show();
            } else {
                F(i4 - 1);
            }
            return true;
        }
        if (itemId == R.id.action_volume_up) {
            int i5 = this.f11783C.f1126e;
            if (i5 >= 9) {
                Toast.makeText(this, "Max volume level has been reached.", 0).show();
            } else {
                F(i5 + 1);
            }
            return true;
        }
        if (itemId == R.id.action_settings) {
            startActivity(new Intent(this, (Class<?>) SettingsActivity.class));
            return true;
        }
        if (itemId != R.id.action_about) {
            return super.onOptionsItemSelected(menuItem);
        }
        startActivity(new Intent(getApplicationContext(), (Class<?>) AboutActivity.class));
        return true;
    }

    @Override // e.AbstractActivityC1805l, android.app.Activity
    public final void onPause() {
        super.onPause();
    }

    @Override // android.app.Activity
    public final boolean onPrepareOptionsMenu(Menu menu) {
        MenuItem findItem = menu.findItem(R.id.action_start);
        MenuItem findItem2 = menu.findItem(R.id.action_settings);
        MenuItem findItem3 = menu.findItem(R.id.action_mute);
        if (this.f11788H == 0) {
            findItem.setTitle(R.string.action_name_start);
            findItem.setIcon(R.drawable.ic_action_start);
            findItem2.setEnabled(true);
            findItem2.getIcon().setAlpha(255);
        } else {
            findItem.setTitle(R.string.action_name_stop);
            findItem.setIcon(R.drawable.ic_action_stop);
            findItem2.setEnabled(false);
            findItem2.getIcon().setAlpha(125);
        }
        if (this.f11798R.f != 1) {
            findItem3.setTitle(R.string.action_name_unmute);
            findItem3.setIcon(R.drawable.ic_action_unmute);
        } else {
            findItem3.setTitle(R.string.action_name_mute);
            findItem3.setIcon(R.drawable.ic_action_mute);
        }
        return true;
    }

    @Override // e.AbstractActivityC1805l, androidx.activity.l, android.app.Activity
    public final void onRequestPermissionsResult(int i3, String[] strArr, int[] iArr) {
        super.onRequestPermissionsResult(i3, strArr, iArr);
        if (i3 != 0 || iArr.length == 0) {
            return;
        }
        StringBuilder sb = new StringBuilder();
        int i4 = 0;
        for (int i5 : iArr) {
            if (i5 != 0) {
                sb.append("\n\t- ");
                sb.append(strArr[i4]);
            }
            i4++;
        }
        if (sb.toString().length() <= 0) {
            G();
            return;
        }
        D("Permissions", "Please go to Settings to grant below permissions to work" + sb.toString());
    }

    @Override // e.AbstractActivityC1805l, android.app.Activity
    public final void onResume() {
        super.onResume();
        if (this.f11783C.f == 1 && this.f11789I) {
            P v3 = v();
            v3.getClass();
            C0114a c0114a = new C0114a(v3);
            c0114a.g(this.f11790J);
            c0114a.d(false);
            this.f11789I = false;
        }
        if (this.f11783C.f == 1 || this.f11789I) {
            return;
        }
        this.f11790J = new b();
        P v4 = v();
        v4.getClass();
        C0114a c0114a2 = new C0114a(v4);
        c0114a2.e(R.id.fragment_container_view, this.f11790J, null, 1);
        c0114a2.f1980p = true;
        c0114a2.d(false);
        this.f11789I = true;
    }

    @Override // androidx.activity.l, x.k, android.app.Activity
    public final void onSaveInstanceState(Bundle bundle) {
        bundle.putBoolean("state_waitingForBluetoothOn", this.f11796P);
        bundle.putBoolean("state_waitingForWifiOn", this.f11797Q);
        super.onSaveInstanceState(bundle);
    }
}
