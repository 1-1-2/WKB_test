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
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import com.miclink.open.BuildConfig
import com.miclink.open.OpusEncoder
import com.miclink.open.R

/**
 * AboutScreen -- 关于与帮助页：使用说明 / 协议说明 / 版本 / 开源许可。
 *
 * 从主页搬来的"使用说明"落在这里：导航重构后主页只保留操作元素，
 * 首次使用的指引 + 协议出处 + 版本号集中到本页。
 */
@Composable
fun AboutScreen(modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // ---- 使用说明 ----
        InfoCard(title = stringResource(R.string.usage_title)) {
            Text(
                text = stringResource(R.string.usage_body),
                style = MaterialTheme.typography.bodyMedium
            )
        }

        // ---- 协议说明 ----
        InfoCard(title = stringResource(R.string.about_protocol)) {
            Text(
                text = stringResource(R.string.about_protocol_body),
                style = MaterialTheme.typography.bodyMedium
            )
            Spacer(Modifier.height(6.dp))
            Text(
                text = stringResource(R.string.protocol_note),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        // ---- 版本 ----
        InfoCard(title = stringResource(R.string.about_version)) {
            // remember：nativeVersion() 走 JNI，且会触发 native 库加载，不该随重组反复调
            val opusVersion = remember { OpusEncoder.libraryVersion() }
            VersionRow(
                label = stringResource(R.string.about_app_version),
                value = "${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})"
            )
            VersionRow(
                label = stringResource(R.string.about_opus_version),
                value = opusVersion
            )
        }

        // ---- 开源许可 ----
        InfoCard(title = stringResource(R.string.about_licenses)) {
            Text(
                text = stringResource(R.string.about_licenses_intro),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(8.dp))
            LICENSES.forEach { license ->
                Row(modifier = Modifier.padding(vertical = 3.dp)) {
                    Text(
                        text = license.name,
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.width(150.dp)
                    )
                    Text(
                        text = license.license,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@Composable
private fun InfoCard(title: String, content: @Composable () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer
        ),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    Icons.Filled.Info,
                    contentDescription = null,
                    modifier = Modifier.size(16.dp)
                )
                Spacer(Modifier.width(6.dp))
                Text(text = title, style = MaterialTheme.typography.titleSmall)
            }
            Spacer(Modifier.height(8.dp))
            content()
        }
    }
}

@Composable
private fun VersionRow(label: String, value: String) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp)
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.width(150.dp)
        )
        Text(text = value, style = MaterialTheme.typography.bodySmall)
    }
}

/** 一条开源许可信息。 */
private data class LicenseEntry(val name: String, val license: String)

/**
 * 开源许可清单。
 *
 * 只列**运行时真正进入 APK** 的依赖（AndroidX/Compose/Kotlin/material-kolor/libopus），
 * 不列 Gradle 插件等构建期组件 —— 那是构建环境的事，随源码分发的只有这些。
 */
private val LICENSES = listOf(
    LicenseEntry("AndroidX / Jetpack Compose", "Apache License 2.0"),
    LicenseEntry("Kotlin 标准库", "Apache License 2.0"),
    LicenseEntry("material-kolor (jordond)", "MIT License"),
    LicenseEntry("libopus (Xiph.Org)", "BSD-style License"),
    LicenseEntry("WO Mic 协议（参考实现）", "见 docs/protocol 规格文档")
)
