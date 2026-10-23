package com.miclink.open.ui.screen

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.miclink.open.MainViewModel
import com.miclink.open.R
import com.miclink.open.theme.AppPalette
import com.miclink.open.theme.PaletteStyle
import com.miclink.open.theme.ThemeMode
import com.miclink.open.theme.buildColorScheme
import com.miclink.open.theme.resolveForDevice
import com.miclink.open.theme.supportsDynamicColor

/**
 * AppearanceScreen -- 外观设置页：主题模式单选 + 配色色板（实时预览）+ AMOLED 开关。
 *
 * 所有开关**即时生效**：ViewModel 的 onTheme*Changed 同步写 SharedPreferences，
 * WomicTheme 随状态重组换 ColorScheme（不重建 Activity，无闪烁，验收 #1/#3）。
 *
 * 色板的"实时预览"分两层：
 *  1. 顶部大预览卡 —— 用当前生效的完整 ColorScheme 渲染一个迷你界面；
 *  2. 每个色板磁贴 —— 用**该色板自己**生成的方案画 4 个色点，点之前就能看到效果。
 */
@Composable
fun AppearanceScreen(vm: MainViewModel, modifier: Modifier = Modifier) {
    val state = vm.themeState
    val dark = when (state.mode) {
        ThemeMode.SYSTEM -> isSystemInDarkTheme()
        ThemeMode.LIGHT -> false
        ThemeMode.DARK -> true
    }
    val dynamicSupported = supportsDynamicColor()
    val effectivePalette = state.palette.resolveForDevice(dynamicSupported)
    val context = LocalContext.current

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // ---- 实时预览（当前生效方案）----
        val effectiveScheme = remember(state.mode, effectivePalette, state.amoled, dark) {
            buildColorScheme(context, state.mode, effectivePalette, state.amoled, dark)
        }
        LivePreviewCard(
            scheme = effectiveScheme,
            caption = "${stringResource(themeModeLabel(state.mode))} · ${
                stringResource(paletteLabel(effectivePalette))
            }"
        )

        // ---- 主题模式（单选）----
        SectionTitle(stringResource(R.string.theme_section_mode))
        ThemeMode.entries.forEach { mode ->
            ModeRow(
                label = stringResource(themeModeLabel(mode)),
                selected = state.mode == mode,
                onClick = { vm.onThemeModeChanged(mode) }
            )
        }

        // ---- 配色 ----
        SectionTitle(stringResource(R.string.theme_section_palette))

        if (dynamicSupported) {
            DynamicPaletteRow(
                selected = state.palette == AppPalette.Dynamic,
                onClick = { vm.onThemePaletteChanged(AppPalette.Dynamic) }
            )
        } else {
            // 低版本没有动态取色：明说不支持，并告知当前回退到哪一档
            Text(
                text = stringResource(
                    R.string.theme_dynamic_unsupported,
                    stringResource(paletteLabel(effectivePalette))
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        // 预置色板：4 活泼 + 4 低饱和，两列排布；每块磁贴自带该方案的色点预览
        val presets = remember { AppPalette.PRESETS }
        val tileSchemes = remember(presets, dark) {
            presets.associateWith { buildColorScheme(context, ThemeMode.SYSTEM, it, false, dark) }
        }
        PaletteGroup(
            title = stringResource(R.string.theme_group_vibrant),
            presets = presets.filter { it.style == PaletteStyle.VIBRANT },
            schemes = tileSchemes,
            selectedId = (state.palette as? AppPalette.Seed)?.id,
            onClick = { vm.onThemePaletteChanged(it) }
        )
        PaletteGroup(
            title = stringResource(R.string.theme_group_muted),
            presets = presets.filter { it.style == PaletteStyle.NEUTRAL },
            schemes = tileSchemes,
            selectedId = (state.palette as? AppPalette.Seed)?.id,
            onClick = { vm.onThemePaletteChanged(it) }
        )

        // ---- AMOLED 纯黑 ----
        SectionTitle(stringResource(R.string.theme_section_amoled))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.theme_section_amoled),
                    style = MaterialTheme.typography.bodyLarge
                )
                Text(
                    text = if (dark) {
                        stringResource(R.string.theme_amoled_hint)
                    } else {
                        stringResource(R.string.theme_amoled_unavailable)
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = state.amoled,
                onCheckedChange = { vm.onThemeAmoledChanged(it) },
                // 浅色下开纯黑会让文字与背景一起变黑，直接禁止并给出说明
                enabled = dark
            )
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary
    )
}

/** 主题模式单选行。 */
@Composable
private fun ModeRow(label: String, selected: Boolean, onClick: () -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 2.dp)
    ) {
        RadioButton(selected = selected, onClick = onClick)
        Spacer(Modifier.width(4.dp))
        Text(text = label, style = MaterialTheme.typography.bodyLarge)
    }
}

/** 动态取色入口（整行，磁贴放不下说明文案）。 */
@Composable
private fun DynamicPaletteRow(selected: Boolean, onClick: () -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 2.dp)
    ) {
        RadioButton(selected = selected, onClick = onClick)
        Spacer(Modifier.width(4.dp))
        Column {
            Text(
                text = stringResource(R.string.palette_dynamic),
                style = MaterialTheme.typography.bodyLarge
            )
            Text(
                text = stringResource(R.string.palette_dynamic_hint),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

/** 一组预置色板（两列）。 */
@Composable
private fun PaletteGroup(
    title: String,
    presets: List<AppPalette.Seed>,
    schemes: Map<AppPalette.Seed, ColorScheme>,
    selectedId: String?,
    onClick: (AppPalette.Seed) -> Unit
) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            text = title,
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        // 两列排布：weight 分行，避免 LazyGrid 嵌套在垂直滚动列里
        presets.chunked(2).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { seed ->
                    PaletteTile(
                        seed = seed,
                        scheme = schemes[seed],
                        selected = seed.id == selectedId,
                        onClick = { onClick(seed) },
                        modifier = Modifier.weight(1f)
                    )
                }
                // 奇数行补位，保证两块磁贴等宽
                if (row.size == 1) {
                    Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}

/** 单个色板磁贴：4 个色点（该方案自己生成的颜色）+ 名称 + 选中态。 */
@Composable
private fun PaletteTile(
    seed: AppPalette.Seed,
    scheme: ColorScheme?,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val shape = RoundedCornerShape(12.dp)
    Column(
        modifier = modifier
            .clip(shape)
            .border(
                width = if (selected) 2.dp else 1.dp,
                color = if (selected) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.outlineVariant
                },
                shape = shape
            )
            .clickable(onClick = onClick)
            .padding(10.dp)
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (scheme != null) {
                Swatch(scheme.primary)
                Swatch(scheme.secondary)
                Swatch(scheme.tertiary)
                Swatch(scheme.surfaceContainerHighest)
            } else {
                // 方案尚未生成（首帧）：用种子色占位，避免磁贴抖动
                Swatch(seed.seedColor)
                Swatch(seed.seedColor)
                Swatch(seed.seedColor)
                Swatch(seed.seedColor)
            }
        }
        Spacer(Modifier.height(6.dp))
        Text(
            text = stringResource(seed.labelRes),
            style = MaterialTheme.typography.labelLarge
        )
    }
}

@Composable
private fun Swatch(color: Color) {
    Box(
        Modifier
            .size(16.dp)
            .clip(CircleShape)
            .background(color)
    )
}

/**
 * 实时预览卡：用传入方案渲染一个迷你界面。
 *
 * 套一层 MaterialTheme 覆盖子树配色 —— 这样"点哪个色板，预览卡就变成哪套"，
 * 而不需要先把设置应用到全局（设置本身就是即时生效的，这里预览的是当前选择）。
 */
@Composable
private fun LivePreviewCard(scheme: ColorScheme, caption: String) {
    MaterialTheme(colorScheme = scheme) {
        Card(
            colors = CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceContainer
            ),
            modifier = Modifier.fillMaxWidth()
        ) {
            Column(
                modifier = Modifier.padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = caption,
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = stringResource(R.string.app_name),
                        style = MaterialTheme.typography.titleMedium
                    )
                    Spacer(Modifier.weight(1f))
                    Surface(
                        shape = RoundedCornerShape(50),
                        color = MaterialTheme.colorScheme.tertiaryContainer
                    ) {
                        Text(
                            text = "LIVE",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onTertiaryContainer,
                            modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp)
                        )
                    }
                }
                Text(
                    text = "192.168.1.5:8125",
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Button(onClick = {}) {
                    Text(stringResource(R.string.action_start))
                }
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Swatch(MaterialTheme.colorScheme.primary)
                    Swatch(MaterialTheme.colorScheme.secondary)
                    Swatch(MaterialTheme.colorScheme.tertiary)
                    Swatch(MaterialTheme.colorScheme.surfaceContainerHighest)
                }
            }
        }
    }
}

/** 主题模式 -> 展示名资源。 */
private fun themeModeLabel(mode: ThemeMode): Int = when (mode) {
    ThemeMode.SYSTEM -> R.string.theme_mode_system
    ThemeMode.LIGHT -> R.string.theme_mode_light
    ThemeMode.DARK -> R.string.theme_mode_dark
}

/** 调色板 -> 展示名资源。 */
private fun paletteLabel(palette: AppPalette): Int = when (palette) {
    AppPalette.Dynamic -> R.string.palette_dynamic
    is AppPalette.Seed -> palette.labelRes
}
