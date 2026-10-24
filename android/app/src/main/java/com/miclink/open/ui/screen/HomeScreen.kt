package com.miclink.open.ui.screen

import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.miclink.open.MainUiState
import com.miclink.open.MainViewModel
import com.miclink.open.MicCaptureService
import com.miclink.open.R
import com.miclink.open.theme.ThemeMode
import com.miclink.open.ui.component.VoiceMeter
import com.miclink.open.ui.theme.WomicTheme

/**
 * HomeScreen -- 主页（由原 MainActivity 内联的 MainScreen 搬出）。
 *
 * 布局沿用 MicYou 风格：常用设置平铺，不套多层设置菜单；顶部状态卡集中展示
 * 连接状态 / 手机地址 / 统计 / 电平。原贴在底部的"使用说明"卡已移入
 * [AboutScreen]（导航重构后主页只留操作相关元素）。
 *
 * 一次性错误/消息弹窗不在这里：已上移到 AppNav 的应用级弹窗（覆盖在当前
 * 目的地之上），所有页面共用（同一事实一处实现）。
 * 麦克风 / 音源 / 系统效果等采集选项在抽屉的"采集设置"页（[CaptureScreen]）。
 *
 * @param vm 主界面状态容器；
 * @param onStartClicked 启动服务（含权限申请流程，由 Activity 实现）；
 * @param onStopClicked 停止服务。
 */
@Composable
fun HomeScreen(
    vm: MainViewModel,
    onStartClicked: () -> Unit,
    onStopClicked: () -> Unit
) {
    val state = vm.uiState

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // ---- 状态卡 ----
        StatusCard(state)

        // ---- 主操作 ----
        Button(
            onClick = { if (state.isRunning) onStopClicked() else onStartClicked() },
            modifier = Modifier
                .fillMaxWidth()
                .height(56.dp),
            enabled = state.isRunning || !state.isStreaming
        ) {
            Icon(
                imageVector = if (state.isRunning) Icons.Filled.Stop else Icons.Filled.PlayArrow,
                contentDescription = null,
                modifier = Modifier.size(20.dp)
            )
            Spacer(Modifier.width(8.dp))
            Text(
                text = stringResource(
                    if (state.isRunning) R.string.action_stop else R.string.action_start
                ),
                style = MaterialTheme.typography.titleMedium
            )
        }

        // ---- 平铺设置（不套嵌套层级）----
        Text(
            text = stringResource(R.string.group_settings),
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.primary
        )

        // 控制端口
        OutlinedTextField(
            value = state.controlPortText,
            onValueChange = { vm.onControlPortTextChanged(it) },
            label = { Text(stringResource(R.string.label_control_port)) },
            supportingText = { Text(stringResource(R.string.hint_control_port)) },
            singleLine = true,
            enabled = !state.isRunning,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            isError = state.controlPortText.toIntOrNull()?.let { it !in 1..65535 } ?: true,
            modifier = Modifier.fillMaxWidth()
        )

        // 音量
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = stringResource(R.string.label_volume),
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.width(96.dp)
                )
                Slider(
                    value = state.volume.toFloat(),
                    onValueChange = { vm.onVolumeChanged(it.toInt()) },
                    valueRange = 0f..255f,
                    steps = 254,
                    enabled = !state.isRunning,
                    modifier = Modifier.weight(1f)
                )
                Text(
                    text = state.volume.toString(),
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.width(40.dp)
                )
            }
            Text(
                text = stringResource(R.string.hint_volume),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        // 调试音
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth()
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.label_debug_tone),
                    style = MaterialTheme.typography.bodyLarge
                )
                Text(
                    text = stringResource(R.string.hint_debug_tone),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = state.debugTone,
                onCheckedChange = { vm.onDebugToneChanged(it) },
                enabled = !state.isRunning
            )
        }

        // 采集选项入口提示（麦克风 / 音源 / 系统效果在抽屉的"采集设置"页）
        Text(
            text = stringResource(R.string.capture_location_hint),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

/** 状态卡：连接状态 + 手机地址 + 统计 + 电平（原 MainActivity 内联组件，整体搬出）。 */
@Composable
private fun StatusCard(state: MainUiState) {
    Card {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = state.state,
                    style = MaterialTheme.typography.titleLarge
                )
                Spacer(Modifier.weight(1f))
                val (container, content) = statusColors(state.state)
                AssistChip(
                    onClick = {},
                    enabled = false,
                    colors = AssistChipDefaults.assistChipColors(
                        containerColor = container,
                        labelColor = content
                    ),
                    label = { Text(if (state.isStreaming) "LIVE" else "WO MIC") }
                )
            }

            Spacer(Modifier.height(8.dp))

            // 手机地址（PC 端要填的）
            Text(
                text = stringResource(R.string.phone_address),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Text(
                text = state.listeningAddress.ifBlank { "—" },
                style = MaterialTheme.typography.headlineSmall,
                fontFamily = FontFamily.Monospace
            )
            Text(
                text = stringResource(R.string.phone_address_hint),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(Modifier.height(8.dp))

            // 统计行
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                StatItem(stringResource(R.string.stat_packets), state.packets.toString())
                StatItem(
                    stringResource(R.string.stat_bytes),
                    stringResource(R.string.stat_kb, (state.bytes / 1024).toInt())
                )
                StatItem(stringResource(R.string.stat_failures), state.failures.toString())
            }

            Spacer(Modifier.height(8.dp))

            // 电平（RMS / Peak 双条 + dB 读数 + 状态徽标 + 平滑衰减）
            VoiceMeter(rmsDb = state.rmsDb, peakDb = state.peakDb)

            if (state.detail.isNotBlank()) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = state.detail,
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
    }
}

@Composable
private fun StatItem(label: String, value: String) {
    Column {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(text = value, style = MaterialTheme.typography.titleMedium)
    }
}

/** 状态 -> chip 配色（容器色, 内容色）。 */
@Composable
private fun statusColors(state: String): Pair<Color, Color> {
    val scheme = MaterialTheme.colorScheme
    return when (state) {
        MicCaptureService.STATE_STREAMING -> scheme.tertiaryContainer to scheme.onTertiaryContainer
        MicCaptureService.STATE_CONNECTED -> scheme.primaryContainer to scheme.onPrimaryContainer
        MicCaptureService.STATE_LISTENING -> scheme.secondaryContainer to scheme.onSecondaryContainer
        MicCaptureService.STATE_ERROR -> scheme.errorContainer to scheme.onErrorContainer
        else -> scheme.surfaceVariant to scheme.onSurfaceVariant
    }
}

@Preview(showBackground = true)
@Composable
private fun HomeScreenPreview() {
    WomicTheme(mode = ThemeMode.LIGHT) {
        HomeScreen(vm = MainViewModel(), onStartClicked = {}, onStopClicked = {})
    }
}
