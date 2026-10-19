# Womic Open -- Android 采集端 ProGuard / R8 规则
#
# 本工程几乎不使用反射，JNI 入口类 OpusEncoder 必须保留 native 方法名。

# 保留 JNI 类及其 native 方法（类名与方法名参与 JNI 符号解析，不能被混淆/裁剪）
-keep class com.womic.open.OpusEncoder {
    native <methods>;
    *;
}

# 保留前台服务（由 AndroidManifest 声明，被系统反射创建）
-keep class com.womic.open.MicCaptureService { *; }

# 保留 Activity（由 Intent 启动）
-keep class com.womic.open.MainActivity { *; }

# 保留 SharedPreferences 用到的常量字段名（无反射，但避免被优化掉便于调试）
-keepclassmembers class com.womic.open.Prefs { *** *; }

# Kotlin 元数据与注解（R8 默认处理，这里显式声明避免 -dontwarn 缺失导致告警）
-keepattributes RuntimeVisibleAnnotations,AnnotationDefault,Signature,InnerClasses,EnclosingMethod
-dontwarn kotlin.**
