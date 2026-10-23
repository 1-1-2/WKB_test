package com.miclink.open.theme

/**
 * 主题模式：跟随系统 / 强制浅色 / 强制深色。
 *
 * 为什么不直接持久化一个"是否深色"的布尔值：
 * "跟随系统"是一种**意图**，不是状态。系统深浅色会随时间/场景切换，
 * 存布尔值会把意图压扁成快照，之后系统切换时 App 不再跟随（缺陷级体验问题）。
 * 因此持久化枚举名，运行时再结合 `isSystemInDarkTheme()` 解析最终明暗。
 */
enum class ThemeMode {
    /** 跟随系统深色设置（默认）。 */
    SYSTEM,

    /** 强制浅色（夜间也保持亮色，例如强光环境下看电平）。 */
    LIGHT,

    /** 强制深色（配合 AMOLED 纯黑省电）。 */
    DARK;

    companion object {
        /** 默认跟随系统，与 Material 3 的默认行为一致。 */
        val DEFAULT: ThemeMode = SYSTEM

        /**
         * 按持久化名解析；无法识别（旧版本数据/脏数据）时回落默认值。
         * 与 [Prefs] 加载运行参数时的"非法值回落默认"策略保持一致。
         */
        fun fromName(name: String?): ThemeMode =
            entries.firstOrNull { it.name == name } ?: DEFAULT
    }
}
