package com.miclink.open.theme

import androidx.compose.material3.ColorScheme
import androidx.compose.ui.graphics.Color
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.JUnit4

/**
 * 主题系统的纯 JVM 单元测试（无 Android 依赖，跑在 :app:test）。
 *
 * 覆盖任务书 T05 验收标准里**可机器验证**的部分：
 *  - 8 套自定义配色（4 活泼 + 4 低饱和）× 明暗各自成 Scheme 且互不相同（#1）；
 *  - AMOLED：surface == background == #000000、surfaceContainer* 有梯度、
 *    onSurface 对比度达 WCAG AA（#2）；
 *  - Android 11 及以下请求动态取色时自动回退 Seed 档（#5）。
 *
 * 重启保持（#3）与抽屉交互（#4）依赖 Android 框架，由 instrumentation/手工验收覆盖。
 */
@RunWith(JUnit4::class)
class ThemeColorSchemeTest {

    private val pureBlack = Color(0xFF000000)

    // ---- 预置表（对应验收 #1 的"8 套自定义配色"） ----

    @Test
    fun themePrefs_verifyDefaults_passes() {
        // 与 Prefs.verifyDefaults() 启动自检同源；这里单独验证，改坏预置表时
        // 不用启动 App 就能在 :app:test 抓到
        ThemePrefs.verifyDefaults()
    }

    @Test
    fun presets_areExactlyFourVibrantPlusFourMuted() {
        assertEquals(8, AppPalette.PRESETS.size)
        assertEquals(4, AppPalette.PRESETS.count { it.style == PaletteStyle.VIBRANT })
        assertEquals(4, AppPalette.PRESETS.count { it.style == PaletteStyle.NEUTRAL })
    }

    @Test
    fun presetIds_areUnique_andFallbackSeedIsSeparate() {
        val ids = AppPalette.PRESETS.map { it.id }
        assertEquals(ids.size, ids.toSet().size)
        assertTrue(AppPalette.FALLBACK_SEED.id !in ids)
    }

    @Test
    fun everyPreset_producesDistinctScheme_inBothLightAndDark() {
        val lightPrimaries = AppPalette.PRESETS.map { it.id to schemeOf(it, isDark = false).primary }
        val darkPrimaries = AppPalette.PRESETS.map { it.id to schemeOf(it, isDark = true).primary }
        // 8 套配色互不重色（明/暗各自成组比较）
        assertEquals(8, lightPrimaries.map { it.second }.toSet().size)
        assertEquals(8, darkPrimaries.map { it.second }.toSet().size)
        // 同一套配色的明暗 primary 应不同（生成器对明暗有分化，而不是简单反转）
        AppPalette.PRESETS.forEach { seed ->
            assertNotEquals(
                "预置 ${seed.id} 明暗 primary 不应相同",
                schemeOf(seed, isDark = false).primary,
                schemeOf(seed, isDark = true).primary
            )
        }
    }

    // ---- AMOLED 纯黑（对应验收 #2） ----

    @Test
    fun amoled_surfaceAndBackgroundArePureBlack() {
        AppPalette.PRESETS.forEach { seed ->
            val scheme = buildColorScheme(
                mode = ThemeMode.DARK,
                palette = seed,
                amoled = true,
                isDark = true
            )
            assertEquals("${seed.id} background", pureBlack, scheme.background)
            assertEquals("${seed.id} surface", pureBlack, scheme.surface)
            assertEquals("${seed.id} surfaceDim", pureBlack, scheme.surfaceDim)
        }
    }

    @Test
    fun amoled_containersFormPerceptibleGradient() {
        AppPalette.PRESETS.forEach { seed ->
            val scheme = buildColorScheme(
                mode = ThemeMode.DARK,
                palette = seed,
                amoled = true,
                isDark = true
            )
            val gradient = listOf(
                scheme.surfaceContainerLowest,
                scheme.surfaceContainerLow,
                scheme.surfaceContainer,
                scheme.surfaceContainerHigh,
                scheme.surfaceContainerHighest
            )
            // 1) 单调递增（层越高越亮，卡片才能"浮"在纯黑背景上）
            gradient.zipWithNext { a, b ->
                assertTrue(
                    "${seed.id} 梯度非递增：${a.toArgbString()} -> ${b.toArgbString()}",
                    relativeLuminance(b) > relativeLuminance(a)
                )
            }
            // 2) 每一层都与纯黑背景有可感知差。最低层（surfaceContainerLowest，
            //    用于最凹陷的元素）允许极淡；卡片实际使用的 Low 及以上层
            //    要求亮度差 >= 0.002，保证"卡片浮在背景上"
            gradient.forEachIndexed { index, container ->
                val delta = relativeLuminance(container) - relativeLuminance(pureBlack)
                val minDelta = if (index == 0) 0.0005f else 0.002f
                assertTrue(
                    "${seed.id} 容器色 ${container.toArgbString()} 与背景无法区分" +
                        "（亮度差 $delta < $minDelta）",
                    delta >= minDelta
                )
            }
            // 3) 梯度终点仍压在 #303030 以内（保持"纯黑"观感，不发灰）
            assertTrue(
                "${seed.id} 最亮容器 ${gradient.last().toArgbString()} 超过 #303030",
                relativeLuminance(gradient.last()) <= relativeLuminance(Color(0xFF303030))
            )
        }
    }

    @Test
    fun amoled_onSurfaceMeetsWcagAa() {
        AppPalette.PRESETS.forEach { seed ->
            val scheme = buildColorScheme(
                mode = ThemeMode.DARK,
                palette = seed,
                amoled = true,
                isDark = true
            )
            assertTrue(
                "${seed.id} onSurface 对比度不足 AA",
                contrastRatio(scheme.onSurface, pureBlack) >= AA_CONTRAST
            )
            assertTrue(
                "${seed.id} onSurfaceVariant 对比度不足 AA",
                contrastRatio(scheme.onSurfaceVariant, pureBlack) >= AA_CONTRAST
            )
        }
    }

    @Test
    fun nonAmoled_darkScheme_keepsMaterialSurface() {
        val scheme = buildColorScheme(
            mode = ThemeMode.DARK,
            palette = AppPalette.PRESETS.first(),
            amoled = false,
            isDark = true
        )
        // 未开 AMOLED 时 surface 应保持 Material 生成的深色（非纯黑），
        // 证明 AMOLED 覆写没有污染普通深色主题
        assertNotEquals(pureBlack, scheme.surface)
        assertNotEquals(pureBlack, scheme.background)
    }

    @Test
    fun lightMode_amoledIsIgnored() {
        // 浅色下开 AMOLED 会让文字与背景一起变黑，必须被忽略
        val scheme = buildColorScheme(
            mode = ThemeMode.LIGHT,
            palette = AppPalette.PRESETS.first(),
            amoled = true,
            isDark = false
        )
        assertNotEquals(pureBlack, scheme.surface)
        assertNotEquals(pureBlack, scheme.background)
    }

    // ---- 动态取色回退（对应验收 #5） ----

    @Test
    fun dynamicPalette_fallsBackToSeedWhenDynamicSchemeUnavailable() {
        // Android 11 及以下：调用方拿不到动态取色方案（传 null），应回退到品牌蓝种子
        val fallback = buildColorScheme(
            mode = ThemeMode.SYSTEM,
            palette = AppPalette.Dynamic,
            amoled = false,
            isDark = true
        )
        val brandSeed = buildColorScheme(
            mode = ThemeMode.SYSTEM,
            palette = AppPalette.FALLBACK_SEED,
            amoled = false,
            isDark = true
        )
        assertEquals(brandSeed.primary, fallback.primary)
        assertEquals(brandSeed.surface, fallback.surface)
    }

    @Test
    fun dynamicPalette_usesProvidedDynamicSchemeWhenAvailable() {
        // Android 12+：调用方提供平台动态取色方案，应优先使用（这里用任意方案冒充）
        val provided = schemeOf(AppPalette.PRESETS[3], isDark = false)
        val scheme = buildColorScheme(
            mode = ThemeMode.SYSTEM,
            palette = AppPalette.Dynamic,
            amoled = false,
            isDark = false,
            dynamicScheme = provided
        )
        assertEquals(provided.primary, scheme.primary)
    }

    @Test
    fun resolveForDevice_switchesDynamicToFallbackOnOldAndroid() {
        assertEquals(
            AppPalette.Dynamic,
            AppPalette.Dynamic.resolveForDevice(dynamicSupported = true)
        )
        assertEquals(
            AppPalette.FALLBACK_SEED,
            AppPalette.Dynamic.resolveForDevice(dynamicSupported = false)
        )
        val seed = AppPalette.PRESETS.first()
        assertEquals(seed, seed.resolveForDevice(dynamicSupported = false))
    }

    // ---- 明暗解析契约 ----

    @Test(expected = IllegalArgumentException::class)
    fun lightModeMustNotResolveToDark() {
        // LIGHT + isDark=true 是调用方解析 bug，buildColorScheme 应直接失败
        buildColorScheme(
            mode = ThemeMode.LIGHT,
            palette = AppPalette.Dynamic,
            amoled = false,
            isDark = true
        )
    }

    @Test
    fun themeMode_fromName_toleratesGarbage() {
        assertEquals(ThemeMode.SYSTEM, ThemeMode.fromName(null))
        assertEquals(ThemeMode.SYSTEM, ThemeMode.fromName("不存在"))
        assertEquals(ThemeMode.DARK, ThemeMode.fromName("DARK"))
        assertEquals(ThemeMode.DEFAULT, ThemeMode.SYSTEM)
    }

    // ---- 工具 ----

    private fun schemeOf(seed: AppPalette.Seed, isDark: Boolean): ColorScheme =
        buildColorScheme(
            mode = if (isDark) ThemeMode.DARK else ThemeMode.LIGHT,
            palette = seed,
            amoled = false,
            isDark = isDark
        )

    /**
     * 调试用的 ARGB 串。Color.value 是 ULong：高 32 位是 sRGB 编码的颜色，
     * 低 32 位是色彩空间 id —— 取高 32 位转 Int 才是 #AARRGGBB。
     */
    private fun Color.toArgbString(): String =
        String.format("#%08X", (value shr 32).toInt())
}
