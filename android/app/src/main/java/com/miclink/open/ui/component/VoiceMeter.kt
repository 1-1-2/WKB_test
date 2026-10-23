package com.miclink.open.ui.component

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.miclink.open.LevelMeter
import com.miclink.open.R
import com.miclink.open.ui.theme.WomicTheme
import kotlin.math.exp
import kotlin.math.pow

/** RMS 条起音时间常数（s）：电平往上冲时要快，跟得上瞬态。 */
private const val RMS_ATTACK_TAU_SEC = 0.03

/** RMS 条回落时间常数（s）：电平往下落时放慢，避免闪烁成"雪花"。 */
private const val RMS_RELEASE_TAU_SEC = 0.25

/** Peak 条保持时长（ms）：峰值停住一会儿才衰减，否则肉眼抓不到峰值。 */
private const val PEAK_HOLD_MS = 1200L

/** Peak 条衰减速度（dB/s）：按分贝线性衰减，符合听感的"匀速下坠"。 */
private const val PEAK_DECAY_DB_PER_SEC = 12.0

/** 峰值接近满量程即视为过载（dBFS）。 */
private const val CLIP_DB = -0.5f

/** 衰减到此电平以下直接归零，避免无限趋近 0 的空转。 */
private const val PEAK_FLOOR = 0.002f

/**
 * VoiceMeter -- 电平表（由原 MainActivity 内联的 LevelBar 升级）。
 *
 * 升级点（对照旧 LevelBar 只有两条裸进度条）：
 *  1. **RMS / Peak 双条**：RMS 用 primary 色，Peak 用 tertiary 色并做峰值保持；
 *  2. **dB 数值**：两条各带等宽字体读数，不用猜进度条含义；
 *  3. **状态徽标**：有信号 / 无信号 / 过载三态，过载用 error 容器色抢眼；
 *  4. **平滑衰减**：单帧循环驱动 —— RMS 指数逼近（起音快、回落慢），
 *     Peak 保持 1.2s 后按 12 dB/s 衰减（经典 VU 表针行为）。
 *
 * 为什么在 UI 侧做平滑而不是在服务侧：服务回调约 20 次/秒（50ms 间隔），
 * 直接画会一顿一顿；在 UI 按帧插值后，60/120Hz 屏幕都是连续动画，
 * 且不增加服务->UI 的回调频率（省电）。
 *
 * @param rmsDb RMS 电平（dBFS）；
 * @param peakDb 峰值电平（dBFS）。
 */
@Composable
fun VoiceMeter(
    rmsDb: Float,
    peakDb: Float,
    modifier: Modifier = Modifier
) {
    // rememberUpdatedState：帧循环里读到的永远是最新目标，无需每次回调重启循环
    val rmsTarget by rememberUpdatedState(LevelMeter.dbToLevel(rmsDb))
    val peakTarget by rememberUpdatedState(LevelMeter.dbToLevel(peakDb))

    var rmsShown by remember { mutableFloatStateOf(0f) }
    var peakShown by remember { mutableFloatStateOf(0f) }
    var peakHoldUntil by remember { mutableLongStateOf(0L) }

    LaunchedEffect(Unit) {
        var lastFrame = 0L
        while (true) {
            val now = withFrameMillis { it }
            if (lastFrame != 0L) {
                val dt = (now - lastFrame).coerceAtLeast(0L) / 1000.0
                if (dt > 0.0) {
                    // RMS：向目标指数逼近，起音/回落用不同时间常数
                    val tau = if (rmsTarget > rmsShown) RMS_ATTACK_TAU_SEC else RMS_RELEASE_TAU_SEC
                    rmsShown += ((rmsTarget - rmsShown) * (1.0 - exp(-dt / tau))).toFloat()
                    // Peak：信号在就跟踪；信号掉了先保持，超时后按 dB 线性衰减
                    if (peakTarget >= peakShown) {
                        peakShown = peakTarget
                        peakHoldUntil = now + PEAK_HOLD_MS
                    } else if (now > peakHoldUntil) {
                        peakShown *= 10.0.pow(-PEAK_DECAY_DB_PER_SEC / 20.0 * dt).toFloat()
                        if (peakShown < PEAK_FLOOR) {
                            peakShown = 0f
                        }
                    }
                }
            }
            lastFrame = now
        }
    }

    Column(modifier) {
        MeterRow(
            label = stringResource(R.string.vm_rms),
            level = rmsShown,
            db = rmsDb,
            barColor = MaterialTheme.colorScheme.primary
        )
        Spacer(Modifier.height(6.dp))
        MeterRow(
            label = stringResource(R.string.vm_peak),
            level = peakShown,
            db = peakDb,
            barColor = MaterialTheme.colorScheme.tertiary
        )
        Spacer(Modifier.height(6.dp))
        SignalBadge(rmsDb = rmsDb, peakDb = peakDb)
    }
}

/** 单条电平：标签 + 进度条 + dB 读数。 */
@Composable
private fun MeterRow(
    label: String,
    level: Float,
    db: Float,
    barColor: Color
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.width(44.dp)
        )
        LinearProgressIndicator(
            progress = { level.coerceIn(0f, 1f) },
            modifier = Modifier
                .weight(1f)
                .height(8.dp),
            color = barColor,
            // 轨道色取 surfaceContainerHighest：AMOLED 梯度下也能与条子区分
            trackColor = MaterialTheme.colorScheme.surfaceContainerHighest
        )
        Text(
            text = stringResource(R.string.vm_db_format, db),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            textAlign = TextAlign.End,
            modifier = Modifier
                .width(76.dp)
                .padding(start = 8.dp)
        )
    }
}

/** 状态徽标：有信号 / 无信号 / 过载。 */
@Composable
private fun SignalBadge(rmsDb: Float, peakDb: Float) {
    val scheme = MaterialTheme.colorScheme
    val (labelRes, container, content) = when {
        peakDb >= CLIP_DB ->
            Triple(R.string.vm_clip, scheme.errorContainer, scheme.onErrorContainer)
        rmsDb > LevelMeter.DB_FLOOR ->
            Triple(R.string.vm_signal, scheme.tertiaryContainer, scheme.onTertiaryContainer)
        else ->
            Triple(R.string.vm_silent, scheme.surfaceVariant, scheme.onSurfaceVariant)
    }
    Surface(shape = RoundedCornerShape(50), color = container) {
        Text(
            text = stringResource(labelRes),
            style = MaterialTheme.typography.labelSmall,
            color = content,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp)
        )
    }
}

@Preview(showBackground = true)
@Composable
private fun VoiceMeterPreview() {
    WomicTheme {
        Column(Modifier.padding(16.dp)) {
            VoiceMeter(rmsDb = -18.5f, peakDb = -6.2f)
            Spacer(Modifier.height(12.dp))
            VoiceMeter(rmsDb = LevelMeter.SILENCE_DB, peakDb = LevelMeter.SILENCE_DB)
        }
    }
}
