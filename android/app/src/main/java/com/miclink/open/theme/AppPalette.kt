package com.miclink.open.theme

import androidx.annotation.StringRes
import androidx.compose.ui.graphics.Color
import com.miclink.open.R

/**
 * 调色板生成风格：活泼（高饱和）/ 低饱和。
 *
 * 直接复用 material-kolor 的 `PaletteStyle` 会把领域模型绑死到第三方库的枚举
 * （R12 的回退方案就要跟着改），因此这里定义自己的两档语义，映射在 ColorSchemes.kt。
 */
enum class PaletteStyle {
    /** 活泼：保留种子色的饱和感，强调辨识度。 */
    VIBRANT,

    /** 低饱和：压淡彩度，长时间盯电平/文字不累。 */
    NEUTRAL
}

/**
 * AppPalette -- 应用配色方案。
 *
 * 两种来源：
 *  1. [Dynamic]：Android 12+ 取系统壁纸的动态取色（Material You）；
 *  2. [Seed]：以固定种子色经 material-kolor 生成完整 ColorScheme。
 */
sealed interface AppPalette {

    /**
     * 持久化标识。Dynamic 固定为 [ID_DYNAMIC]，Seed 用预置 id。
     * 暴露为接口成员而不是各自定义，保证 SharedPreferences 读写只有一处实现。
     */
    val id: String

    /** 动态取色：Android 12+ 从壁纸生成主题。 */
    data object Dynamic : AppPalette {
        override val id: String = ID_DYNAMIC
    }

    /**
     * 种子色调色板。
     *
     * @param id 持久化键，**一经发布不得修改**（用户已保存的设置按 id 找回）；
     * @param labelRes 展示名（字符串资源，便于本地化）；
     * @param seedColor 种子色（不透明）；
     * @param style 生成风格，决定最终彩度走向。
     */
    data class Seed(
        override val id: String,
        @param:StringRes val labelRes: Int,
        val seedColor: Color,
        val style: PaletteStyle
    ) : AppPalette

    companion object {

        /** 动态取色的持久化 id。 */
        const val ID_DYNAMIC: String = "dynamic"

        /** 默认动态取色（Android 11 及以下由 ColorSchemes 回退到 [FALLBACK_SEED]）。 */
        val DEFAULT: AppPalette = Dynamic

        /**
         * 预置调色板：4 活泼 + 4 低饱和（顺序即"外观"页展示顺序）。
         *
         * id 命名规则：前缀体现风格（vibrant_ 与 muted_ 两族），便于从持久化值
         * 直接读出风格，不需要反查 [PRESETS]。
         */
        val PRESETS: List<Seed> = listOf(
            // ---- 活泼 ----
            Seed(
                id = "vibrant_coral",
                labelRes = R.string.palette_coral,
                seedColor = Color(0xFFFF6D3A),
                style = PaletteStyle.VIBRANT
            ),
            Seed(
                id = "vibrant_magenta",
                labelRes = R.string.palette_magenta,
                seedColor = Color(0xFFE91E63),
                style = PaletteStyle.VIBRANT
            ),
            Seed(
                id = "vibrant_amber",
                labelRes = R.string.palette_amber,
                seedColor = Color(0xFFFFB300),
                style = PaletteStyle.VIBRANT
            ),
            Seed(
                id = "vibrant_teal",
                labelRes = R.string.palette_teal,
                seedColor = Color(0xFF00BCD4),
                style = PaletteStyle.VIBRANT
            ),
            // ---- 低饱和 ----
            Seed(
                id = "muted_mist",
                labelRes = R.string.palette_mist,
                seedColor = Color(0xFF607D9B),
                style = PaletteStyle.NEUTRAL
            ),
            Seed(
                id = "muted_sage",
                labelRes = R.string.palette_sage,
                seedColor = Color(0xFF7A9070),
                style = PaletteStyle.NEUTRAL
            ),
            Seed(
                id = "muted_rose",
                labelRes = R.string.palette_rose,
                seedColor = Color(0xFFA87C82),
                style = PaletteStyle.NEUTRAL
            ),
            Seed(
                id = "muted_stone",
                labelRes = R.string.palette_stone,
                seedColor = Color(0xFF757575),
                style = PaletteStyle.NEUTRAL
            )
        )

        /**
         * Android 11 及以下请求动态取色时的回退种子。
         *
         * 取启动图标的品牌蓝（drawable/ic_launcher.xml 的 #1E88E5）而不是随手挑一个：
         * 低版本用户看到的默认观感应与桌面图标一致，否则"默认主题"没有品牌锚点。
         * 注意它**不属于** [PRESETS]（预置表固定 4+4，见 ThemePrefs.verifyDefaults）。
         */
        val FALLBACK_SEED: Seed = Seed(
            id = "brand_blue",
            labelRes = R.string.palette_brand_blue,
            seedColor = Color(0xFF1E88E5),
            style = PaletteStyle.VIBRANT
        )

        /** 按持久化 id 解析；未知 id 回落 [DEFAULT]（与 ThemeMode.fromName 同策略）。 */
        fun fromId(id: String?): AppPalette {
            if (id == null) return DEFAULT
            if (id == ID_DYNAMIC) return Dynamic
            return PRESETS.firstOrNull { it.id == id } ?: DEFAULT
        }
    }
}

/**
 * 解析设备上**实际生效**的调色板。
 *
 * 动态取色自 Android 12（API 31）才有；低版本上用户若选了 [AppPalette.Dynamic]，
 * 自动回退到 [AppPalette.FALLBACK_SEED]（验收 #5：回退到 Seed 档且不崩）。
 * 这比"低版本隐藏动态取色入口"更好：设置可以跨设备恢复/迁移，语义不丢。
 */
fun AppPalette.resolveForDevice(dynamicSupported: Boolean): AppPalette =
    if (this is AppPalette.Dynamic && !dynamicSupported) AppPalette.FALLBACK_SEED else this
