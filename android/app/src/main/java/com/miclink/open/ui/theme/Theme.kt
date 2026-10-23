package com.miclink.open.ui.theme

import android.app.Activity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat
import com.miclink.open.theme.AppPalette
import com.miclink.open.theme.ThemeMode
import com.miclink.open.theme.buildColorScheme

/**
 * WomicTheme -- 应用主题（Material 3 + 动态取色 / 种子调色板 / AMOLED 纯黑）。
 *
 * 取代原先内联在 MainActivity 里的 `WomicWifiClientTheme`：那个实现只有
 * "系统深色 -> 平台动态取色，否则默认配色"一条路，无法表达 8 套调色板与
 * AMOLED 诉求。配色决策全部委托 [buildColorScheme]，本 composable 只负责：
 *   1. 按 mode + 系统深色解析最终明暗；
 *   2. 用 remember 缓存 ColorScheme —— 明暗/调色板/AMOLED 任一变化才重新生成
 *      （material-kolor 生成涉及 HCT 色彩空间换算，不是零成本，且切换要"即时
 *      生效无闪烁"，验收 #1，靠状态驱动重组而不是重建 Activity）；
 *   3. 同步系统栏（状态栏/导航栏）颜色与图标明暗，避免深色主题顶着一个亮色状态栏。
 *
 * @param mode 主题模式；
 * @param palette 配色（Dynamic 或预置 Seed）；
 * @param amoled AMOLED 纯黑（仅深色生效）；
 * @param content 内容。
 */
@Composable
fun WomicTheme(
    mode: ThemeMode = ThemeMode.SYSTEM,
    palette: AppPalette = AppPalette.Dynamic,
    amoled: Boolean = false,
    content: @Composable () -> Unit
) {
    val dark = when (mode) {
        ThemeMode.SYSTEM -> isSystemInDarkTheme()
        ThemeMode.LIGHT -> false
        ThemeMode.DARK -> true
    }
    val context = LocalContext.current
    val colorScheme = remember(mode, palette, amoled, dark) {
        buildColorScheme(context, mode, palette, amoled, dark)
    }

    // 系统栏跟随主题：非 edge-to-edge 窗口下直接着色；API 35+ 平台已强制
    // edge-to-edge，statusBarColor 为无害空操作（仅图标明暗设置仍生效）。
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as? Activity)?.window
            if (window != null) {
                WindowCompat.getInsetsController(window, view).apply {
                    isAppearanceLightStatusBars = !dark
                    isAppearanceLightNavigationBars = !dark
                }
                @Suppress("DEPRECATION")
                window.statusBarColor = colorScheme.background.toArgb()
                @Suppress("DEPRECATION")
                window.navigationBarColor = colorScheme.background.toArgb()
            }
        }
    }

    MaterialTheme(colorScheme = colorScheme, typography = Typography(), content = content)
}
