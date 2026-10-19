package com.womic.open

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * Permissions -- 运行时权限申请（麦克风 + Android 13 通知）。
 *
 * 说明：
 *   - `RECORD_AUDIO` 是危险权限，必须在运行时申请；缺了它 AudioRecord 会直接初始化失败；
 *   - `POST_NOTIFICATIONS` 自 Android 13（API 33）起需要申请，否则前台服务通知不可见
 *     （服务仍能跑，但用户看不到停止入口，体验很差，所以一起申请）；
 *   - `INTERNET` / `FOREGROUND_SERVICE` / `WAKE_LOCK` 是普通权限，安装时自动授予。
 */
object Permissions {

    /** 权限请求码（onRequestPermissionsResult 中回传）。 */
    const val REQ_CAPTURE = 1001

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

    /** 是否还能再弹系统权限框（用户勾选"不再询问"后返回 false）。 */
    fun canAskAgain(activity: Activity, permission: String): Boolean {
        return ActivityCompat.shouldShowRequestPermissionRationale(activity, permission) ||
            ContextCompat.checkSelfPermission(activity, permission) != PackageManager.PERMISSION_GRANTED
    }

    /**
     * 申请缺失权限。
     *
     * @return true 表示已全部拥有（无需弹框）；false 表示已发起申请，
     *         结果在 Activity 的 onRequestPermissionsResult 中回调。
     */
    fun requestIfNeeded(activity: Activity): Boolean {
        val need = missing(activity)
        if (need.isEmpty()) {
            return true
        }
        ActivityCompat.requestPermissions(activity, need.toTypedArray(), REQ_CAPTURE)
        return false
    }

    /**
     * 处理申请结果。
     *
     * @return true 表示本请求码是采集权限且**全部通过**；false 表示被拒绝（或部分拒绝）。
     */
    fun handleResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ): Boolean {
        if (requestCode != REQ_CAPTURE) {
            return false
        }
        if (grantResults.isEmpty()) {
            return false
        }
        for (result in grantResults) {
            if (result != PackageManager.PERMISSION_GRANTED) {
                return false
            }
        }
        return permissions.isNotEmpty()
    }

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
