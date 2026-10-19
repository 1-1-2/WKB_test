package com.miclink.open

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.SocketException
import java.nio.charset.StandardCharsets

/**
 * Discovery -- 局域网内广播发现 Windows 服务端。
 *
 * 协议（与 server/src/discovery.cpp 严格对应）：
 *   请求（手机 -> 广播）：`MICLINK-DISCOVER 1`
 *   应答（服务端 -> 单播）：`MICLINK-HERE 1 <audio_port> <rate> <channels> <name>`
 *     - audio_port：服务端实际监听的 UDP 音频端口（默认 5004）
 *     - rate：端点采样率（默认 48000）
 *     - channels：端点声道数（默认 1）
 *     - name：发现名，服务端会把空格替换成 '_'（默认 `MicLink_Virtual_Microphone`）
 *
 * 前置条件：服务端必须用 `--discovery on`（或 ini 里 `discovery = true`）开启发现服务，
 * 否则不会有任何应答（服务端默认关闭）。
 *
 * 本类的方法**必须在子线程调用**（会阻塞等待应答）。
 */
class Discovery {

    /** 发现到的服务端。 */
    data class Server(
        val address: InetAddress,
        val audioPort: Int,
        val sampleRate: Int,
        val channels: Int,
        val name: String
    ) {
        override fun toString(): String =
            "$name @ ${address.hostAddress}:$audioPort（${sampleRate}Hz / ${channels}ch）"
    }

    companion object {
        private const val TAG = "Discovery"

        /**
         * 发现端口，与 server/src/config.h 的 discovery_port 默认值一致。
         *
         * 唯一事实来源是 app/build.gradle.kts 的
         * buildConfigField("DEFAULT_DISCOVERY_PORT")，这里只做转发（缺陷 D-11）：
         * 避免 5005 在 BuildConfig 与 Kotlin 常量里各写一份。
         */
        val DEFAULT_DISCOVERY_PORT: Int = BuildConfig.DEFAULT_DISCOVERY_PORT

        /** 查询报文（服务端以前缀 "MICLINK-DISCOVER" 匹配）。 */
        private const val QUERY = "MICLINK-DISCOVER 1"

        /** 应答前缀。 */
        private const val REPLY_PREFIX = "MICLINK-HERE"

        /** 单轮扫描默认等待时长。 */
        const val DEFAULT_TIMEOUT_MS = 1500

        /** 接收缓冲。 */
        private const val BUF_SIZE = 512
    }

    /** 最近一次错误信息；null 表示无错误。 */
    @Volatile
    var lastError: String? = null
        private set

    /**
     * 扫描一次。
     *
     * @param discoveryPort 发现端口（默认 5005）
     * @param timeoutMs     等待应答的上限
     * @return 发现到的服务端列表（可能为空）；出错时返回空列表并置 [lastError]
     */
    fun scan(discoveryPort: Int = DEFAULT_DISCOVERY_PORT, timeoutMs: Int = DEFAULT_TIMEOUT_MS): List<Server> {
        lastError = null
        val socket = try {
            DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                soTimeout = timeoutMs.coerceIn(200, 10000)
                bind(InetSocketAddress(discoveryPort))
            }
        } catch (e: SocketException) {
            lastError = "创建发现 socket 失败：${e.message}（服务端可能已经占用 :$discoveryPort）"
            Log.e(TAG, lastError!!, e)
            return emptyList()
        }

        val found = LinkedHashMap<String, Server>()
        try {
            val queryBytes = QUERY.toByteArray(StandardCharsets.US_ASCII)
            var sentAny = false
            for (broadcast in broadcastAddresses()) {
                try {
                    socket.send(DatagramPacket(queryBytes, queryBytes.size, broadcast, discoveryPort))
                    sentAny = true
                } catch (e: Exception) {
                    Log.w(TAG, "向 $broadcast 发送发现请求失败：${e.message}")
                }
            }
            if (!sentAny) {
                lastError = "没有可用的广播地址（请确认已连上 Wi-Fi 且未开启网络隔离）"
                return emptyList()
            }

            val deadline = System.currentTimeMillis() + timeoutMs
            val buf = ByteArray(BUF_SIZE)
            while (System.currentTimeMillis() < deadline) {
                val packet = DatagramPacket(buf, buf.size)
                try {
                    socket.receive(packet)
                } catch (e: java.net.SocketTimeoutException) {
                    break
                } catch (e: Exception) {
                    lastError = "接收应答异常：${e.message}"
                    break
                }
                val text = String(packet.data, 0, packet.length, StandardCharsets.US_ASCII).trim()
                // packet.address 是 Java getter，不是稳定变量，先取到局部 val 再判空
                val from = packet.address
                if (from != null) {
                    val server = parseReply(text, from)
                    if (server != null) {
                        found[from.hostAddress ?: ""] = server
                    }
                }
            }
        } finally {
            try {
                socket.close()
            } catch (t: Throwable) {
                Log.w(TAG, "关闭发现 socket 异常：${t.message}")
            }
        }
        return found.values.toList()
    }

    /**
     * 解析服务端应答。
     *
     * @return 解析成功返回 [Server]；不是 MICLINK 应答或格式不合规则返回 null
     */
    fun parseReply(text: String, from: InetAddress?): Server? {
        val parts = text.split(Regex("\\s+")).filter { it.isNotEmpty() }
        if (parts.size < 5) {
            return null
        }
        if (parts[0] != REPLY_PREFIX) {
            return null
        }
        if (from == null) {
            return null
        }
        val version = parts[1].toIntOrNull() ?: return null
        if (version != 1) {
            Log.w(TAG, "发现应答版本不匹配：$version")
            return null
        }
        val audioPort = parts[2].toIntOrNull() ?: return null
        val rate = parts[3].toIntOrNull() ?: return null
        val channels = parts[4].toIntOrNull() ?: return null
        if (audioPort !in 1..65535) return null
        if (rate !in listOf(16000, 44100, 48000)) return null
        if (channels !in 1..2) return null
        // 服务端把名字里的空格替换成了 '_'，这里还原成空格以便显示
        val name = if (parts.size >= 6) parts.subList(5, parts.size).joinToString(" ").replace('_', ' ') else "MicLink Server"
        return Server(from, audioPort, rate, channels, name)
    }

    /** 枚举本机所有可用于广播的地址（含 255.255.255.255）。 */
    private fun broadcastAddresses(): List<InetAddress> {
        val result = mutableListOf<InetAddress>()
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces() ?: return result
            while (interfaces.hasMoreElements()) {
                val nif = interfaces.nextElement()
                if (!nif.isUp || nif.isLoopback) {
                    continue
                }
                for (ia in nif.interfaceAddresses) {
                    val bcast = ia.broadcast
                    if (bcast != null) {
                        result.add(bcast)
                    }
                }
            }
        } catch (e: SocketException) {
            Log.w(TAG, "枚举网卡失败：${e.message}")
        }
        // 兜底：受限广播地址（部分 ROM 不允许，失败会被上层忽略）
        result.add(InetAddress.getByName("255.255.255.255"))
        return result
    }
}
