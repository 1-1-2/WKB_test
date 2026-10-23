package com.miclink.open.theme

import android.content.Context
import android.content.SharedPreferences
import com.miclink.open.Prefs

/**
 * 主题设置的持久化快照。
 *
 * 定义在 theme 包而不是 MainViewModel：本包的 [ThemePrefs] 需要读写它，
 * 而 MainViewModel 在根包且依赖 theme 包 —— 放这里可避免循环依赖
 * （MainViewModel -> ThemePrefs -> ThemeState -> MainViewModel）。
 */
data class ThemeState(
    val mode: ThemeMode = ThemePrefs.DEFAULT_MODE,
    val palette: AppPalette = ThemePrefs.DEFAULT_PALETTE,
    val amoled: Boolean = ThemePrefs.DEFAULT_AMOLED
)

/**
 * ThemePrefs -- 主题设置的 SharedPreferences 持久化（4 键）。
 *
 * 复用 [Prefs] 的同一个文件（`womic_wifi_client_prefs`）：主题与运行参数同属
 * "用户设置"，分文件会让备份（allowBackup）/ 清除数据的行为分裂。
 * 运行参数字段仍由 [Prefs] 自己管理，本类只负责主题 4 键：
 *
 * | 键             | 值                            | 说明                                   |
 * |----------------|-------------------------------|----------------------------------------|
 * | `theme_mode`   | ThemeMode.name                | SYSTEM / LIGHT / DARK                  |
 * | `theme_palette`| AppPalette.id                 | "dynamic" 或预置种子 id                |
 * | `theme_style`  | PaletteStyle.name             | 与 id **解耦**持久化（见 load）        |
 * | `theme_amoled` | Boolean                       | 仅深色生效（见 ColorSchemes）          |
 *
 * style 单独存一键的理由：预置表将来可能调整某个 id 的风格（例如珊瑚橙从活泼
 * 改成低饱和），已保存风格的偏好不应被改表静默覆盖 —— id 负责找回"是哪一档"，
 * style 负责记住"用户当年选的是哪种彩度"。
 */
class ThemePrefs(private val prefs: SharedPreferences) {

    /** 读取主题设置；缺项/脏值一律回落默认值（与 Prefs.load() 同策略）。 */
    fun load(): ThemeState {
        return ThemeState(
            mode = ThemeMode.fromName(prefs.getString(KEY_MODE, null)),
            palette = resolvePalette(
                id = prefs.getString(KEY_PALETTE, null),
                styleName = prefs.getString(KEY_STYLE, null)
            ),
            amoled = prefs.getBoolean(KEY_AMOLED, DEFAULT_AMOLED)
        )
    }

    /** 写入主题设置（apply 异步落盘，重启后保持，验收 #3）。 */
    fun save(state: ThemeState) {
        val seed = state.palette as? AppPalette.Seed
        prefs.edit()
            .putString(KEY_MODE, state.mode.name)
            .putString(KEY_PALETTE, state.palette.id)
            .putString(KEY_STYLE, seed?.style?.name ?: "")
            .putBoolean(KEY_AMOLED, state.amoled)
            .apply()
    }

    /**
     * id + style -> 调色板。
     *
     * style 以持久化值为准（预置表风格只是"当前推荐"）；持久化 style 无法识别时
     * 才回落到预置表自带风格。
     */
    private fun resolvePalette(id: String?, styleName: String?): AppPalette {
        if (id == null) return DEFAULT_PALETTE
        if (id == AppPalette.ID_DYNAMIC) return AppPalette.Dynamic
        val preset = AppPalette.PRESETS.firstOrNull { it.id == id } ?: return DEFAULT_PALETTE
        val style = PaletteStyle.entries.firstOrNull { it.name == styleName }
        return if (style == null || style == preset.style) preset else preset.copy(style = style)
    }

    companion object {
        private const val KEY_MODE = "theme_mode"
        private const val KEY_PALETTE = "theme_palette"
        private const val KEY_STYLE = "theme_style"
        private const val KEY_AMOLED = "theme_amoled"

        /** 默认主题模式：跟随系统。 */
        val DEFAULT_MODE: ThemeMode = ThemeMode.DEFAULT

        /** 默认配色：动态取色（低版本自动回退 Seed 档）。 */
        val DEFAULT_PALETTE: AppPalette = AppPalette.DEFAULT

        /** 默认关闭 AMOLED（纯黑是高对比偏好，不是大众默认）。 */
        const val DEFAULT_AMOLED: Boolean = false

        /**
         * 打开主题存储（复用 [Prefs.FILE_NAME] 指代的同一个文件）。
         *
         * 正常路径走 [Prefs.loadTheme] / [Prefs.saveTheme] 委托；本入口供
         * 测试或将来独立使用主题存储的调用方使用。
         */
        fun open(context: Context): ThemePrefs = ThemePrefs(
            context.applicationContext.getSharedPreferences(
                Prefs.FILE_NAME,
                Context.MODE_PRIVATE
            )
        )

        /**
         * 默认值自检，对齐 [Prefs.verifyDefaults] 的"崩在启动期"思路（缺陷 D-11）：
         * 一旦有人改坏默认值或改乱预置表，启动即崩，而不是静默丢主题。
         *
         * 由 [Prefs.verifyDefaults] 统一调用（启动自检入口保持唯一）。
         */
        fun verifyDefaults() {
            require(DEFAULT_MODE == ThemeMode.SYSTEM) {
                "DEFAULT_MODE($DEFAULT_MODE) 应为 SYSTEM（跟随系统）"
            }
            require(DEFAULT_PALETTE == AppPalette.Dynamic) {
                "DEFAULT_PALETTE 应为 Dynamic（Android 12+ 动态取色）"
            }
            require(!DEFAULT_AMOLED) {
                "DEFAULT_AMOLED 应为 false（纯黑是偏好项，不是默认）"
            }
            // 预置表必须恰好 4 活泼 + 4 低饱和（任务书 T05 文件表）
            require(AppPalette.PRESETS.size == 8) {
                "预置调色板应为 8 套（4 活泼 + 4 低饱和），实际 ${AppPalette.PRESETS.size}"
            }
            require(AppPalette.PRESETS.count { it.style == PaletteStyle.VIBRANT } == 4) {
                "活泼档应为 4 套"
            }
            require(AppPalette.PRESETS.count { it.style == PaletteStyle.NEUTRAL } == 4) {
                "低饱和档应为 4 套"
            }
            // id 是持久化键：重复会让两个调色板互相"顶号"，用户设置随机漂移
            val ids = AppPalette.PRESETS.map { it.id }
            require(ids.size == ids.toSet().size) {
                "预置调色板 id 重复：${ids.groupingBy { it }.eachCount().filter { it.value > 1 }}"
            }
            require(AppPalette.FALLBACK_SEED.id !in ids) {
                "回退种子 id(${AppPalette.FALLBACK_SEED.id}) 不应出现在预置表中"
            }
            AppPalette.PRESETS.forEach { preset ->
                require(preset.labelRes != 0) { "预置 ${preset.id} 缺少展示名资源" }
                require(preset.seedColor.alpha == 1f) {
                    "预置 ${preset.id} 种子色必须不透明（alpha=${preset.seedColor.alpha}）"
                }
            }
        }
    }
}
