package com.miclink.open

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.isSystemInDarkTheme
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
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat

/**
 * MainActivity -- WO Mic 采集端 UI（Compose Material 3，设置项平铺）。
 *
 * 布局风格参考 MicYou：常用设置直接平铺在主页面，不套多层设置菜单；
 * 顶部一张状态卡集中展示连接状态 / 手机地址 / 统计 / 电平，主操作按钮紧随其后。
 *
 * 与服务的通信：绑定 [MicCaptureService] + [MicCaptureService.CaptureStatusListener]
 * 回调（主线程），状态汇入 [MainViewModel]；统计由 500ms 轮询补齐。
 */
class MainActivity : ComponentActivity() {

    companion object {
        private const val TAG = "MainActivity"
        private const val UI_REFRESH_INTERVAL_MS = 500L
    }

    private val vm: MainViewModel by viewModels()

    /**
     * 运行时权限申请（Activity Result API）。
     *
     * 回调里**重新检查实际权限状态**而不是信任回调 Map：用户直接划掉对话框时
     * 部分 ROM 会回传空 Map（`all {}` 对空集合恒为 true，会误判为全部授予）。
     */
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
        if (Permissions.allGranted(this)) {
            startCaptureService()
        } else {
            vm.onError(getString(R.string.permission_denied, Permissions.describeMissing(this)))
        }
    }

    private var service: MicCaptureService? = null
    private var bound = false
    private val mainHandler = Handler(Looper.getMainLooper())

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val svc = (binder as? MicCaptureService.LocalBinder)?.getService()
            service = svc
            bound = svc != null
            svc?.setStatusListener(statusListener)
            svc?.let {
                vm.onServiceState(it.currentState(), it.currentDetail())
                vm.onListeningAddress(it.listeningAddress())
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
            bound = false
        }
    }

    private val statusListener = object : MicCaptureService.CaptureStatusListener {
        override fun onState(state: String, detail: String) {
            vm.onServiceState(state, detail)
            service?.let { vm.onListeningAddress(it.listeningAddress()) }
        }

        override fun onLevel(rmsDb: Float, peakDb: Float) {
            vm.onLevel(rmsDb, peakDb)
        }

        override fun onError(message: String) {
            vm.onError(message)
        }
    }

    /** 定时刷新统计与监听地址。 */
    private val uiRefreshRunnable = object : Runnable {
        override fun run() {
            vm.pollStats(service)
            service?.let { vm.onListeningAddress(it.listeningAddress()) }
            mainHandler.postDelayed(this, UI_REFRESH_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 启动自检（缺陷 D-11）：默认值必须与 build.gradle.kts 的 BuildConfig 一致
        // 且落在合法区间，否则直接崩在启动阶段，而不是"连上了却没声音"。
        Prefs.verifyDefaults()
        vm.loadConfig(Prefs(this))

        setContent {
            WomicWifiClientTheme {
                Surface {
                    MainScreen(
                        vm = vm,
                        onStartClicked = { onStartClicked() },
                        onStopClicked = { onStopClicked() }
                    )
                }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        bindService(
            Intent(this, MicCaptureService::class.java),
            serviceConnection,
            Context.BIND_AUTO_CREATE
        )
    }

    override fun onResume() {
        super.onResume()
        mainHandler.post(uiRefreshRunnable)
    }

    override fun onPause() {
        mainHandler.removeCallbacks(uiRefreshRunnable)
        super.onPause()
    }

    override fun onStop() {
        service?.setStatusListener(null)
        if (bound) {
            try {
                unbindService(serviceConnection)
            } catch (t: Throwable) {
                Log.w(TAG, "unbindService 失败：${t.message}")
            }
            bound = false
        }
        service = null
        super.onStop()
    }

    // 权限结果经 [permissionLauncher]（Activity Result API）回调，无需重写
    // onRequestPermissionsResult（activity 1.13 起该回调已废弃）。

    // ------------------------------------------------------------------
    // 动作
    // ------------------------------------------------------------------

    private fun onStartClicked() {
        val config = vm.readConfig()
        if (config == null) {
            vm.onError(getString(R.string.error_invalid_port))
            return
        }
        Prefs(this).save(config)
        if (!Permissions.allGranted(this)) {
            permissionLauncher.launch(Permissions.missing(this).toTypedArray())
            return
        }
        startCaptureService()
    }

    private fun startCaptureService() {
        val config = vm.readConfig() ?: return
        val intent = MicCaptureService.buildStartIntent(this, config)
        try {
            ContextCompat.startForegroundService(this, intent)
        } catch (t: Throwable) {
            Log.e(TAG, "启动服务失败", t)
            vm.onError(getString(R.string.error_start_service, t.message ?: ""))
            return
        }
        if (!bound) {
            bindService(
                Intent(this, MicCaptureService::class.java),
                serviceConnection,
                Context.BIND_AUTO_CREATE
            )
        }
    }

    private fun onStopClicked() {
        try {
            startService(MicCaptureService.buildStopIntent(this))
        } catch (t: Throwable) {
            Log.w(TAG, "停止服务失败：${t.message}")
        }
    }
}

// ----------------------------------------------------------------------
// 主题
// ----------------------------------------------------------------------

@Composable
fun WomicWifiClientTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit
) {
    val colorScheme = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }
        darkTheme -> darkColorScheme()
        else -> lightColorScheme()
    }
    MaterialTheme(colorScheme = colorScheme, content = content)
}

// ----------------------------------------------------------------------
// 主界面
// ----------------------------------------------------------------------

@Composable
fun MainScreen(
    vm: MainViewModel,
    onStartClicked: () -> Unit,
    onStopClicked: () -> Unit
) {
    val state = vm.uiState

    // 错误提示（一次性）：展示中 -> 关闭时消费
    val error = state.error
    if (error != null) {
        AlertDialog(
            onDismissRequest = { vm.consumeError() },
            title = { Text(stringResource(R.string.app_name)) },
            text = { Text(error) },
            confirmButton = {
                TextButton(onClick = { vm.consumeError() }) {
                    Text("知道了")
                }
            }
        )
    }

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

        // ---- 使用说明 ----
        Card(
            colors = CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceVariant
            )
        ) {
            Column(Modifier.padding(12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        Icons.Filled.Info,
                        contentDescription = null,
                        modifier = Modifier.size(16.dp)
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(
                        text = stringResource(R.string.usage_title),
                        style = MaterialTheme.typography.titleSmall
                    )
                }
                Spacer(Modifier.height(6.dp))
                Text(
                    text = stringResource(R.string.usage_body),
                    style = MaterialTheme.typography.bodySmall
                )
                Spacer(Modifier.height(6.dp))
                Text(
                    text = stringResource(R.string.protocol_note),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

/** 状态卡：连接状态 + 手机地址 + 统计 + 电平。 */
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

            // 电平（RMS / Peak 双条）
            LevelBar(label = "RMS", level = LevelMeter.dbToLevel(state.rmsDb))
            Spacer(Modifier.height(4.dp))
            LevelBar(label = "Peak", level = LevelMeter.dbToLevel(state.peakDb))
            Spacer(Modifier.height(4.dp))
            Text(
                text = stringResource(R.string.level_label, state.rmsDb, state.peakDb),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

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

@Composable
private fun LevelBar(label: String, level: Float) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.width(40.dp)
        )
        LinearProgressIndicator(
            progress = { level.coerceIn(0f, 1f) },
            modifier = Modifier
                .weight(1f)
                .height(6.dp)
        )
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
private fun MainScreenPreview() {
    WomicWifiClientTheme(darkTheme = false) {
        MainScreen(vm = MainViewModel(), onStartClicked = {}, onStopClicked = {})
    }
}
