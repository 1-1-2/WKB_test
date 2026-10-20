package s1;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Parcel;
import android.text.TextUtils;
import android.util.Log;
import androidx.preference.EditTextPreference;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import com.wo.voice2.R;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.concurrent.Executors;
import org.json.JSONObject;
import t1.InterfaceC2037c;
import v1.x;

/* loaded from: classes.dex */
public class i implements C1.c, u1.h, K0.b, X1.c, X1.d {
    public static i f;

    /* renamed from: g, reason: collision with root package name */
    public static i f13451g;

    /* renamed from: h, reason: collision with root package name */
    public static i f13452h;

    /* renamed from: i, reason: collision with root package name */
    public static i f13453i;

    /* renamed from: e, reason: collision with root package name */
    public final /* synthetic */ int f13454e;

    public /* synthetic */ i(int i3) {
        this.f13454e = i3;
    }

    public i(JSONObject jSONObject) {
        this.f13454e = 6;
        jSONObject.optString("billingPeriod");
        jSONObject.optString("priceCurrencyCode");
        jSONObject.optString("formattedPrice");
        jSONObject.optLong("priceAmountMicros");
        jSONObject.optInt("recurrenceMode");
        jSONObject.optInt("billingCycleCount");
    }

    private final void f() {
    }

    private final void g() {
    }

    public static void h(Context context) {
        x.d(context);
        synchronized (i.class) {
            try {
                if (f == null) {
                    p.a(context);
                    i iVar = new i(0);
                    context.getApplicationContext();
                    f = iVar;
                }
            } catch (Throwable th) {
                throw th;
            }
        }
    }

    /* JADX WARN: Code restructure failed: missing block: B:36:0x0049, code lost:
    
        if (java.lang.Character.isHighSurrogate(r5) != false) goto L35;
     */
    /* JADX WARN: Code restructure failed: missing block: B:65:0x0086, code lost:
    
        if (java.lang.Character.isLowSurrogate(r5) != false) goto L60;
     */
    /* JADX WARN: Code restructure failed: missing block: B:69:0x0079, code lost:
    
        if (r11 != false) goto L48;
     */
    /*
        Code decompiled incorrectly, please refer to instructions dump.
        To view partially-correct add '--show-bad-code' argument
    */
    public static boolean k(android.view.inputmethod.InputConnection r7, android.text.Editable r8, int r9, int r10, boolean r11) {
        /*
            Method dump skipped, instructions count: 244
            To view this dump add '--comments-level debug' option
        */
        throw new UnsupportedOperationException("Method not decompiled: s1.i.k(android.view.inputmethod.InputConnection, android.text.Editable, int, int, boolean):boolean");
    }

    public static final m q(PackageInfo packageInfo, m... mVarArr) {
        Signature[] signatureArr = packageInfo.signatures;
        if (signatureArr != null) {
            if (signatureArr.length != 1) {
                Log.w("GoogleSignatureVerifier", "Package has more than one signature.");
                return null;
            }
            n nVar = new n(packageInfo.signatures[0].toByteArray());
            for (int i3 = 0; i3 < mVarArr.length; i3++) {
                if (mVarArr[i3].equals(nVar)) {
                    return mVarArr[i3];
                }
            }
        }
        return null;
    }

    public static final boolean r(PackageInfo packageInfo) {
        PackageInfo packageInfo2;
        boolean z3;
        if (packageInfo != null) {
            if ("com.android.vending".equals(packageInfo.packageName) || "com.google.android.gms".equals(packageInfo.packageName)) {
                ApplicationInfo applicationInfo = packageInfo.applicationInfo;
                z3 = (applicationInfo == null || (applicationInfo.flags & 129) == 0) ? false : true;
            } else {
                z3 = true;
            }
            packageInfo2 = packageInfo;
        } else {
            packageInfo2 = null;
            z3 = true;
        }
        if (packageInfo != null && packageInfo2.signatures != null) {
            if ((z3 ? q(packageInfo2, o.f13463a) : q(packageInfo2, o.f13463a[0])) != null) {
                return true;
            }
        }
        return false;
    }

    @Override // X1.d
    public boolean a() {
        return true;
    }

    @Override // C1.c
    public int b(Context context, String str) {
        return C1.e.a(context, str);
    }

    @Override // X1.c
    public int c(short[] sArr, int i3, short[] sArr2, int i4) {
        if (i4 < i3) {
            return 0;
        }
        System.arraycopy(sArr, 0, sArr2, 0, i3);
        return i3;
    }

    @Override // X1.c
    public void close() {
        int i3 = this.f13454e;
    }

    @Override // X1.d
    public int d(short[] sArr, int i3, byte[] bArr) {
        int i4 = 0;
        for (int i5 = 0; i5 < i3; i5++) {
            int i6 = i4 + 1;
            short s3 = sArr[i5];
            bArr[i4] = (byte) (s3 >> 8);
            i4 += 2;
            bArr[i6] = (byte) (s3 & 255);
        }
        return i3 * 2;
    }

    @Override // C1.c
    public int e(Context context, String str, boolean z3) {
        return C1.e.d(context, str, z3);
    }

    @Override // g2.a
    public Object get() {
        switch (this.f13454e) {
            case 9:
                return new C0.b(Executors.newSingleThreadExecutor(), 1);
            case 10:
            case 11:
            case 12:
            default:
                return new R0.b(0);
            case 13:
                return "com.google.android.datatransport.events";
            case 14:
                return Integer.valueOf(P0.l.f947h);
            case 15:
                return P0.a.f;
            case 16:
                return new R0.b(1);
        }
    }

    public Signature[] i(PackageManager packageManager, String str) {
        return packageManager.getPackageInfo(str, 64).signatures;
    }

    public int j(Object obj) {
        return ((F.i) obj).c;
    }

    public boolean l(Object obj) {
        return ((F.i) obj).f356d;
    }

    @Override // u1.h
    public void m(InterfaceC2037c interfaceC2037c, O1.f fVar) {
        E1.c cVar = (E1.c) ((E1.b) interfaceC2037c).t();
        E1.g gVar = new E1.g(fVar);
        Parcel obtain = Parcel.obtain();
        obtain.writeInterfaceToken("com.google.android.gms.appset.internal.IAppSetService");
        int i3 = E1.a.f326a;
        obtain.writeInt(1);
        C2.a.W(obtain, C2.a.P(obtain, 20293));
        obtain.writeStrongBinder(gVar);
        Parcel obtain2 = Parcel.obtain();
        try {
            cVar.f327e.transact(1, obtain, obtain2, 0);
            obtain2.readException();
        } finally {
            obtain.recycle();
            obtain2.recycle();
        }
    }

    public byte[] n(X1.a aVar, X1.d dVar, boolean z3, int i3) {
        switch (this.f13454e) {
            case 25:
                if (z3) {
                    Arrays.fill(aVar.f1187a, (short) 0);
                } else {
                    aVar.a(i3);
                }
                short[] sArr = aVar.f1187a;
                byte[] bArr = new byte[1024];
                int d3 = dVar.d(sArr, sArr.length, bArr);
                ByteArrayOutputStream byteArrayOutputStream = new ByteArrayOutputStream(d3 + 6);
                DataOutputStream dataOutputStream = new DataOutputStream(byteArrayOutputStream);
                try {
                    dataOutputStream.writeInt(aVar.f1188b);
                    dataOutputStream.writeShort(d3);
                    dataOutputStream.write(bArr, 0, d3);
                    dataOutputStream.flush();
                    return byteArrayOutputStream.toByteArray();
                } catch (IOException unused) {
                    throw new AssertionError("We should not be here!");
                }
            case 26:
                if (z3) {
                    Arrays.fill(aVar.f1187a, (short) 0);
                } else {
                    aVar.a(i3);
                }
                short[] sArr2 = aVar.f1187a;
                byte[] bArr2 = new byte[1024];
                int d4 = dVar.d(sArr2, sArr2.length, bArr2);
                ByteArrayOutputStream byteArrayOutputStream2 = new ByteArrayOutputStream(d4 + 6);
                DataOutputStream dataOutputStream2 = new DataOutputStream(byteArrayOutputStream2);
                int i4 = 0;
                for (int i5 = 0; i5 < d4; i5++) {
                    try {
                        i4 ^= bArr2[i5];
                    } catch (IOException unused2) {
                        throw new AssertionError("We should not be here!");
                    }
                }
                dataOutputStream2.writeByte(i4);
                dataOutputStream2.writeByte(0);
                dataOutputStream2.writeShort(aVar.f1188b);
                dataOutputStream2.writeShort(d4);
                dataOutputStream2.write(bArr2, 0, d4);
                dataOutputStream2.flush();
                return byteArrayOutputStream2.toByteArray();
            default:
                if (z3) {
                    i3 = 0;
                }
                short[] sArr3 = aVar.f1187a;
                byte[] bArr3 = new byte[1024];
                int d5 = dVar.d(sArr3, sArr3.length, bArr3);
                ByteArrayOutputStream byteArrayOutputStream3 = new ByteArrayOutputStream(d5 + 11);
                DataOutputStream dataOutputStream3 = new DataOutputStream(byteArrayOutputStream3);
                try {
                    dataOutputStream3.writeByte(4);
                    dataOutputStream3.writeByte(0);
                    dataOutputStream3.writeShort(d5 + 7);
                    dataOutputStream3.writeShort(aVar.f1188b);
                    dataOutputStream3.writeInt(aVar.c);
                    dataOutputStream3.writeByte(i3);
                    dataOutputStream3.write(bArr3, 0, d5);
                    dataOutputStream3.flush();
                    return byteArrayOutputStream3.toByteArray();
                } catch (IOException unused3) {
                    throw new AssertionError("It has never been expected!");
                }
        }
    }

    public CharSequence o(Preference preference) {
        switch (this.f13454e) {
            case 19:
                EditTextPreference editTextPreference = (EditTextPreference) preference;
                return TextUtils.isEmpty(editTextPreference.f2188X) ? editTextPreference.f2215e.getString(R.string.not_set) : editTextPreference.f2188X;
            default:
                ListPreference listPreference = (ListPreference) preference;
                return TextUtils.isEmpty(listPreference.B()) ? listPreference.f2215e.getString(R.string.not_set) : listPreference.B();
        }
    }

    public C1.d p(Context context, String str, C1.c cVar) {
        switch (this.f13454e) {
            case 3:
                C1.d dVar = new C1.d();
                int e3 = cVar.e(context, str, true);
                dVar.f209b = e3;
                if (e3 != 0) {
                    dVar.c = 1;
                } else {
                    int b3 = cVar.b(context, str);
                    dVar.f208a = b3;
                    if (b3 != 0) {
                        dVar.c = -1;
                    }
                }
                return dVar;
            default:
                C1.d dVar2 = new C1.d();
                dVar2.f208a = cVar.b(context, str);
                int i3 = 1;
                int e4 = cVar.e(context, str, true);
                dVar2.f209b = e4;
                int i4 = dVar2.f208a;
                if (i4 == 0) {
                    i4 = 0;
                    if (e4 == 0) {
                        i3 = 0;
                        dVar2.c = i3;
                        return dVar2;
                    }
                }
                if (i4 >= e4) {
                    i3 = -1;
                }
                dVar2.c = i3;
                return dVar2;
        }
    }
}
