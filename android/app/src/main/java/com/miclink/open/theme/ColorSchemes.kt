package com.miclink.open.theme

import android.content.Context
import android.os.Build
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.ui.graphics.Color
import com.materialkolor.PaletteStyle as MkPaletteStyle
import com.materialkolor.dynamicColorScheme
import kotlin.math.pow

/** WCAG 2.1 AA：正文文本对比度下限。 */
const val AA_CONTRAST: Float = 4.5f

/**
 * 设备是否支持动态取色（Android 12 / API 31 起的 Material You）。
 *
 * 单独抽成函数而不是在 UI 里散判 `Build.VERSION`：AppearanceScreen 的提示文案、
 * buildColorScheme 的回退判断、单元测试必须用同一个判据，否则会出现
 * "设置页说支持、实际却用了回退色"的分裂。
 */
fun supportsDynamicColor(): Boolean = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S

/**
 * 纯计算版 [buildColorScheme]（不依赖 Context）。
 *
 * 动态取色需要平台 API，由调用方通过 [dynamicScheme] 提供；传 null 表示不可用
 * （Android 11 及以下），此时 [AppPalette.Dynamic] 自动回退到 Seed 档
 * （[AppPalette.FALLBACK_SEED]），保证低版本不崩且仍有品牌一致的观感。
 *
 * @param mode 主题模式。最终明暗以 [isDark] 为准（调用方按 mode + 系统深色解析），
 *             mode 在此用于一致性自检（LIGHT 不应解析出深色）；
 * @param palette 用户选择的配色；
 * @param amoled 是否启用 AMOLED 纯黑（仅深色生效，见 [withAmoled]）；
 * @param isDark 最终明暗；
 * @param dynamicScheme 动态取色方案（Android 12+ 由 Context 版提供）。
 */
fun buildColorScheme(
    mode: ThemeMode,
    palette: AppPalette,
    amoled: Boolean,
    isDark: Boolean,
    dynamicScheme: ColorScheme? = null
): ColorScheme {
    require(!(mode == ThemeMode.LIGHT && isDark)) {
        "LIGHT 模式不应解析出深色配色（mode=$mode, isDark=$isDark），调用方解析逻辑有误"
    }
    val base = when (palette) {
        AppPalette.Dynamic -> dynamicScheme ?: seedScheme(AppPalette.FALLBACK_SEED, isDark)
        is AppPalette.Seed -> seedScheme(palette, isDark)
    }
    // AMOLED 纯黑只对深色有意义：浅色下把背景压黑会让文字与背景一起变黑，直接不可读
    return if (amoled && isDark) base.withAmoled() else base
}

/**
 * Context 版 [buildColorScheme]：按 SDK 自动提供动态取色。
 *
 * Android 12+ 走 `dynamicDarkColorScheme` / `dynamicLightColorScheme`（系统壁纸取色）；
 * Android 11 及以下传 null，由纯计算版回退到 Seed 档。
 */
fun buildColorScheme(
    context: Context,
    mode: ThemeMode,
    palette: AppPalette,
    amoled: Boolean,
    isDark: Boolean
): ColorScheme {
    val dynamicScheme = if (supportsDynamicColor()) {
        if (isDark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
    } else {
        null
    }
    return buildColorScheme(mode, palette, amoled, isDark, dynamicScheme)
}

/** 以种子色经 material-kolor 生成完整 ColorScheme。 */
private fun seedScheme(seed: AppPalette.Seed, isDark: Boolean): ColorScheme =
    dynamicColorScheme(
        seedColor = seed.seedColor,
        isDark = isDark,
        style = when (seed.style) {
            PaletteStyle.VIBRANT -> MkPaletteStyle.Vibrant
            PaletteStyle.NEUTRAL -> MkPaletteStyle.Neutral
        }
    )

/**
 * AMOLED 纯黑覆写（仅深色，由 [buildColorScheme] 在 `amoled && isDark` 时调用）。
 *
 * 设计约束（验收 #2）：
 *  1. `surface == background == #000000` —— OLED 纯黑像素不发光，是最深的"黑"，
 *     同时避免暗灰背景在大暗环境下发青；
 *  2. `surfaceContainer*` 必须保留**可感知的灰度梯度** —— 否则 Card 与页面背景
 *     同色，卡片"消失"。梯度终点压在 #303030 以内，兼顾层级感与纯黑观感；
 *  3. `onSurface` 对比度必须达 WCAG AA —— 生成方案的 onSurface 通常已满足
 *     （近白对纯黑约 15:1），这里仍兜底校验，防止动态取色产出异常中性色。
 */
fun ColorScheme.withAmoled(): ColorScheme = copy(
    background = AmoledColors.PURE_BLACK,
    surface = AmoledColors.PURE_BLACK,
    surfaceDim = AmoledColors.PURE_BLACK,
    surfaceBright = AmoledColors.SURFACE_BRIGHT,
    surfaceContainerLowest = AmoledColors.SURFACE_CONTAINER_LOWEST,
    surfaceContainerLow = AmoledColors.SURFACE_CONTAINER_LOW,
    surfaceContainer = AmoledColors.SURFACE_CONTAINER,
    surfaceContainerHigh = AmoledColors.SURFACE_CONTAINER_HIGH,
    surfaceContainerHighest = AmoledColors.SURFACE_CONTAINER_HIGHEST,
    onSurface = ensureAaForeground(onSurface),
    onSurfaceVariant = ensureAaForeground(onSurfaceVariant)
)

/** AMOLED 梯度常量（单调递增，层间距约 7/255，肉眼可辨但不发灰）。 */
private object AmoledColors {
    val PURE_BLACK = Color(0xFF000000)
    val SURFACE_CONTAINER_LOWEST = Color(0xFF050505)
    val SURFACE_CONTAINER_LOW = Color(0xFF0C0C0C)
    val SURFACE_CONTAINER = Color(0xFF141414)
    val SURFACE_CONTAINER_HIGH = Color(0xFF1C1C1C)
    val SURFACE_CONTAINER_HIGHEST = Color(0xFF262626)
    val SURFACE_BRIGHT = Color(0xFF303030)
}

/** AA 兜底前景色（近白，对纯黑对比度约 17:1）。 */
private val AA_SAFE_FOREGROUND = Color(0xFFECECEC)

/** 对纯黑背景不满足 AA 时替换为兜底前景色。 */
private fun ensureAaForeground(foreground: Color): Color =
    if (contrastRatio(foreground, AmoledColors.PURE_BLACK) >= AA_CONTRAST) {
        foreground
    } else {
        AA_SAFE_FOREGROUND
    }

/** WCAG 2.1 相对亮度（sRGB）。 */
fun relativeLuminance(color: Color): Float {
    fun channel(value: Float): Float =
        if (value <= 0.03928f) {
            value / 12.92f
        } else {
            ((value + 0.055f) / 1.055f).pow(2.4f)
        }
    return 0.2126f * channel(color.red) +
        0.7152f * channel(color.green) +
        0.0722f * channel(color.blue)
}

/** WCAG 2.1 对比度（1.0 ~ 21.0）。供单元测试直接复用，避免测试里另写一份。 */
fun contrastRatio(a: Color, b: Color): Float {
    val la = relativeLuminance(a)
    val lb = relativeLuminance(b)
    val hi = if (la >= lb) la else lb
    val lo = if (la >= lb) lb else la
    return (hi + 0.05f) / (lo + 0.05f)
}
