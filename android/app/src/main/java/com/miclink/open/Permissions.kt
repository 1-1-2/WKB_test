package com.miclink.open

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat

/**
 * Permissions -- 运行时权限申请（麦克风 + Android 13 通知）。
 *
 * 说明：
 *   - `RECORD_AUDIO` 是危险权限，必须在运行时申请；缺了它 AudioRecord 会直接初始化失败；
 *   - `POST_NOTIFICATIONS` 自 Android 13（API 33）起需要申请，否则前台服务通知不可见
 *     （服务仍能跑，但用户看不到停止入口，体验很差，所以一起申请）；
 *   - `INTERNET` / `FOREGROUND_SERVICE` / `WAKE_LOCK` 是普通权限，安装时自动授予。
 *
 * 申请动作统一走 Activity Result API（`ActivityResultContracts.RequestMultiplePermissions`）：
 * 由调用方注册 launcher 并发起，结果回调里重新检查 [allGranted] 判定成败
 * （不信任回调 Map 本身，兼容"用户直接划掉对话框"等返回空 Map 的行为）。
 */
object Permissions {

    /** 采集所需的全部危险权限。 */
    fun requiredPermissions(): List<String> {
        val list = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            list.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        return list
    }

    /** 尚未授予的权限列表。 */
    fun missing(context: Context): List<String> {
        return requiredPermissions().filter {
            ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
        }
    }

    /** 是否全部已授予。 */
    fun allGranted(context: Context): Boolean = missing(context).isEmpty()

    /** 给用户看的权限说明（被拒绝时展示）。 */
    fun describeMissing(context: Context): String {
        val miss = missing(context)
        if (miss.isEmpty()) {
            return "权限已就绪"
        }
        val names = miss.map { nameOf(it) }
        return "缺少权限：${names.joinToString("、")}"
    }

    /** 权限 -> 中文名（避免把 Manifest.permission.* 原文丢给用户）。 */
    fun nameOf(permission: String): String = when (permission) {
        Manifest.permission.RECORD_AUDIO -> "麦克风（RECORD_AUDIO）"
        Manifest.permission.POST_NOTIFICATIONS -> "通知（POST_NOTIFICATIONS）"
        else -> permission
    }
}
