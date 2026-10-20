package com.wo.voice2;

import A.g;
import V1.a;
import V1.f;
import V1.n;
import V1.o;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import java.util.Calendar;

/* loaded from: classes.dex */
public class VoiceService extends Service {

    /* renamed from: j, reason: collision with root package name */
    public static final /* synthetic */ int f11811j = 0;

    /* renamed from: e, reason: collision with root package name */
    public PowerManager.WakeLock f11812e;
    public f f;

    /* renamed from: g, reason: collision with root package name */
    public NotificationChannel f11813g;

    /* renamed from: h, reason: collision with root package name */
    public n f11814h;

    /* renamed from: i, reason: collision with root package name */
    public final a f11815i = new a(this, 1);

    public final void a(int i3, int i4) {
        if (i3 != 1) {
            if (i3 == 2) {
                PowerManager.WakeLock newWakeLock = ((PowerManager) getSystemService("power")).newWakeLock(1, ":WOMic");
                this.f11812e = newWakeLock;
                newWakeLock.acquire();
                b(getString(R.string.notification_listening_text), true);
            } else if (i3 != 3) {
                PowerManager.WakeLock wakeLock = this.f11812e;
                if (wakeLock != null && wakeLock.isHeld()) {
                    this.f11812e.release();
                }
                stopForeground(true);
            } else {
                b(getString(R.string.notification_connected_text), true);
            }
        }
        Intent intent = new Intent("com.wo.voice.VoiceService.ACTION_STATE");
        intent.setPackage("com.wo.voice2");
        intent.putExtra("com.wo.voice.VoiceService.EXTRA_STATE", i3);
        intent.putExtra("com.wo.voice.VoiceService.EXTRA_REASON", i4);
        sendBroadcast(intent);
    }

    public final void b(String str, boolean z3) {
        Notification.Builder builder;
        if (!z3) {
            stopForeground(true);
            return;
        }
        PendingIntent broadcast = PendingIntent.getBroadcast(this, 1, new Intent("com.wo.voice.VoiceService.STOP_SERVER"), 201326592);
        int i3 = Build.VERSION.SDK_INT;
        if (i3 >= 26) {
            if (this.f11813g == null) {
                NotificationChannel c = A1.a.c();
                c.setDescription("This is WO Mic channel");
                c.enableLights(false);
                c.setLightColor(-65536);
                c.enableVibration(false);
                c.setVibrationPattern(new long[]{0, 0, 0, 0, 0, 0, 0, 0, 0});
                c.setShowBadge(true);
                ((NotificationManager) getSystemService("notification")).createNotificationChannel(c);
                this.f11813g = c;
            }
            builder = A1.a.b(this);
        } else {
            builder = new Notification.Builder(this);
        }
        builder.setContentTitle("WO Mic").setContentText(str).setOngoing(true).setWhen(Calendar.getInstance().getTimeInMillis()).setShowWhen(true).setSmallIcon(R.drawable.ic_notification).setColor(getResources().getColor(R.color.notification_color)).setContentIntent(broadcast);
        Notification build = builder.build();
        if (i3 >= 30) {
            startForeground(1, build, 128);
        } else {
            startForeground(1, build);
        }
    }

    @Override // android.app.Service
    public final IBinder onBind(Intent intent) {
        return new o(this);
    }

    @Override // android.app.Service
    public final void onCreate() {
        super.onCreate();
        this.f = f.a(this);
        n nVar = new n(new g(this, 15));
        this.f11814h = nVar;
        nVar.f1148m = this.f.f1126e;
        IntentFilter intentFilter = new IntentFilter();
        intentFilter.addAction("com.wo.voice.VoiceService.STOP_SERVER");
        int i3 = Build.VERSION.SDK_INT;
        a aVar = this.f11815i;
        if (i3 >= 26) {
            registerReceiver(aVar, intentFilter, 4);
        } else {
            registerReceiver(aVar, intentFilter);
        }
        a(0, 0);
    }

    @Override // android.app.Service
    public final void onDestroy() {
        unregisterReceiver(this.f11815i);
        b(null, false);
        this.f11814h.f1153r.g(-1);
        super.onDestroy();
    }

    @Override // android.app.Service
    public final int onStartCommand(Intent intent, int i3, int i4) {
        return 1;
    }
}
