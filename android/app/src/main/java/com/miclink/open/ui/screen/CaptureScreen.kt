package com.miclink.open.ui.screen

import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.miclink.open.AudioDeviceManager
import com.miclink.open.AudioSourceIds
import com.miclink.open.AudioSourceOption
import com.miclink.open.CaptureSettings
import com.miclink.open.MainViewModel
import com.miclink.open.R
import com.miclink.open.theme.ThemeMode
import com.miclink.open.ui.theme.WomicTheme

/**
 * CaptureScreen -- 采集设置页（抽屉导航目的地之一）。
 *
 * 三组选项，全部只用 Android framework 能力（零自研算法）：
 *  1. **音源模式**：六档单选（见 [AudioSourceOption]）；UNPROCESSED 需设备支持
 *     （系统属性），不支持时禁用并说明原因——不能选了没效果；
 *  2. **麦克风**：输入设备单选 + "跟随系统（自动）"；选中项经
 *     `AudioRecord.setPreferredDevice` 应用，实际生效设备以
 *     `AudioRecord.getRoutedDevice` 为准（见 [com.miclink.open.MainUiState.routedDeviceLabel]）；
 *  3. **系统效果**：NS / AEC / AGC 开关，`isAvailable()` 为 false 时禁用并说明。
 *
 * 与主页端口/音量的区别：这些选项**录音中也可改**，变更即时生效（服务重建
 * AudioRecord，可能有半秒左右杂音），并即时落盘（重启后保持）。
 */
@Composable
fun CaptureScreen(vm: MainViewModel, modifier: Modifier = Modifier) {
    val state = vm.uiState
    val context = LocalContext.current
    val manager = remember(context) { AudioDeviceManager(context) }

    // 设备枚举与能力查询：结果只进本地 state（设备列表不是"偏好"，无需持久化），
    // 热插拔经 AudioDeviceCallback 刷新。
    var devices by remember(manager) { mutableStateOf(manager.inputDevices()) }
    val unprocessedSupported = remember(manager) { manager.isUnprocessedSupported() }
    val noiseSuppressorAvailable = remember(manager) { manager.isNoiseSuppressorAvailable() }
    val echoCancelerAvailable = remember(manager) { manager.isEchoCancelerAvailable() }
    val gainControlAvailable = remember(manager) { manager.isGainControlAvailable() }

    DisposableEffect(manager) {
        val callback = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
                devices = manager.inputDevices()
            }

            override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
                devices = manager.inputDevices()
            }
        }
        manager.registerDeviceCallback(callback)
        onDispose { manager.unregisterDeviceCallback(callback) }
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        if (state.debugTone) {
            // 调试音模式不打开 AudioRecord：明说以下选项暂不生效
            // （CI 联调闭环全程跑该模式，这里是唯一显式提示点）
            Text(
                text = stringResource(R.string.capture_debug_tone_note),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.tertiary
            )
        }

        // ---- 音源模式 ----
        SectionTitle(stringResource(R.string.capture_section_source))
        AudioSourceOption.entries.forEach { option ->
            val enabled = option.id != AudioSourceIds.UNPROCESSED || unprocessedSupported
            OptionRow(
                title = stringResource(option.labelRes),
                description = if (enabled) {
                    stringResource(option.descRes)
                } else {
                    stringResource(R.string.capture_source_unprocessed_unsupported)
                },
                selected = state.capture.audioSource == option.id,
                enabled = enabled,
                onClick = { vm.onAudioSourceChanged(option.id) }
            )
        }

        // ---- 麦克风 ----
        SectionTitle(stringResource(R.string.capture_section_mic))
        OptionRow(
            title = stringResource(R.string.capture_mic_auto),
            description = stringResource(R.string.capture_mic_auto_desc),
            selected = state.capture.micDeviceId == CaptureSettings.MIC_DEVICE_AUTO,
            enabled = true,
            onClick = { vm.onMicDeviceChanged(CaptureSettings.MIC_DEVICE_AUTO) }
        )
        devices.forEach { device ->
            OptionRow(
                title = device.label,
                description = null,
                selected = state.capture.micDeviceId == device.id,
                enabled = true,
                onClick = { vm.onMicDeviceChanged(device.id) }
            )
        }
        // 当前实际生效的设备（来自 AudioRecord.getRoutedDevice，而非"用户选了啥"）
        Text(
            text = if (state.isStreaming) {
                state.routedDeviceLabel?.let { stringResource(R.string.capture_mic_routed, it) } ?: "—"
            } else {
                stringResource(R.string.capture_mic_not_recording)
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = stringResource(R.string.capture_mic_hint),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        // ---- 系统降噪 / 回声 / 增益 ----
        SectionTitle(stringResource(R.string.capture_section_effects))
        EffectSwitchRow(
            title = stringResource(R.string.capture_effect_ns),
            description = stringResource(R.string.capture_effect_ns_desc),
            available = noiseSuppressorAvailable,
            checked = state.capture.noiseSuppressor,
            onCheckedChange = { vm.onNoiseSuppressorChanged(it) }
        )
        EffectSwitchRow(
            title = stringResource(R.string.capture_effect_aec),
            description = stringResource(R.string.capture_effect_aec_desc),
            available = echoCancelerAvailable,
            checked = state.capture.echoCanceler,
            onCheckedChange = { vm.onEchoCancelerChanged(it) }
        )
        EffectSwitchRow(
            title = stringResource(R.string.capture_effect_agc),
            description = stringResource(R.string.capture_effect_agc_desc),
            available = gainControlAvailable,
            checked = state.capture.gainControl,
            onCheckedChange = { vm.onGainControlChanged(it) }
        )
        Text(
            text = stringResource(R.string.capture_effect_hint),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

/** 小节标题（与 AppearanceScreen 的 SectionTitle 同风格）。 */
@Composable
private fun SectionTitle(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary
    )
}

/** 单选行（标题 + 可选说明 + RadioButton；禁用态不给点击反馈）。 */
@Composable
private fun OptionRow(
    title: String,
    description: String?,
    selected: Boolean,
    enabled: Boolean,
    onClick: () -> Unit
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(enabled = enabled, onClick = onClick)
            .padding(vertical = 2.dp)
    ) {
        RadioButton(selected = selected, onClick = onClick, enabled = enabled)
        Spacer(Modifier.width(4.dp))
        Column {
            Text(text = title, style = MaterialTheme.typography.bodyLarge)
            if (description != null) {
                Text(
                    text = description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

/**
 * 系统效果开关行：设备不支持（`isAvailable()` = false）时**禁用并说明原因**，
 * 不假装生效；勾选态仍反映已保存的设置（若设备实际拒绝附加，服务会发明确提示）。
 */
@Composable
private fun EffectSwitchRow(
    title: String,
    description: String,
    available: Boolean,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(text = title, style = MaterialTheme.typography.bodyLarge)
            Text(
                text = if (available) {
                    description
                } else {
                    stringResource(R.string.capture_effect_unavailable)
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            enabled = available
        )
    }
}

@Preview(showBackground = true)
@Composable
private fun CaptureScreenPreview() {
    WomicTheme(mode = ThemeMode.LIGHT) {
        CaptureScreen(vm = MainViewModel())
    }
}
