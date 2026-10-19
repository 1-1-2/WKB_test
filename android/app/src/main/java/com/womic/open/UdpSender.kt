package com.womic.open

import android.util.Log
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketException
import java.net.UnknownHostException

/**
 * UdpSender -- 把封好的 womic 包发给 Windows 服务端。
 *
 * 设计要点：
 *   - 一个 DatagramSocket 复用到底，避免每帧创建 socket（会显著抬高抖动与 CPU）；
 *   - 目标地址在 open() 时解析一次并缓存，避免每帧做 DNS（主机名解析可能阻塞）；
 *   - 所有 IOException / SocketException 都被捕获并转成返回值 + lastError，
 *     **不让异常冒泡到采集线程**，否则一次网络抖动就会中断整个采集；
 *   - 连续失败达到阈值后由调用方决定是否停止（本类只负责上报 failureStreak）。
 */
class UdpSender {

    companion object {
        private const val TAG = "UdpSender"
        /**
         * 默认音频端口，与 server/src/config.h 的 listen_port 默认值一致。
         *
         * 唯一事实来源是 app/build.gradle.kts 的
         * buildConfigField("DEFAULT_UDP_PORT")，这里只做转发（缺陷 D-11）。
         */
        val DEFAULT_PORT: Int = BuildConfig.DEFAULT_UDP_PORT
    }

    @Volatile
    private var socket: DatagramSocket? = null

    @Volatile
    private var targetAddress: InetAddress? = null

    @Volatile
    var targetPort: Int = DEFAULT_PORT
        private set

    /** 最近一次错误信息；null 表示一切正常。 */
    @Volatile
    var lastError: String? = null
        private set

    /** 连续发送失败次数（成功后清零）。 */
    @Volatile
    var failureStreak: Int = 0
        private set

    /** 已成功发送的包数。 */
    @Volatile
    var packetsSent: Long = 0L
        private set

    /** 已成功发送的字节数（含 32 字节头部）。 */
    @Volatile
    var bytesSent: Long = 0L
        private set

    val isOpen: Boolean
        get() = socket != null && targetAddress != null

    /**
     * 打开 socket 并解析目标地址。
     *
     * @param host 服务端 IPv4（也可填主机名，但建议直接填 IP，避免 DNS 抖动）
     * @param port 服务端 UDP 监听端口（默认 5004）
     * @return true 成功
     */
    @Synchronized
    fun open(host: String, port: Int): Boolean {
        close()
        lastError = null
        failureStreak = 0

        if (host.isBlank()) {
            lastError = "目标地址为空"
            return false
        }
        if (port !in 1..65535) {
            lastError = "端口非法: $port"
            return false
        }

        val address = try {
            InetAddress.getByName(host.trim())
        } catch (e: UnknownHostException) {
            lastError = "无法解析主机: $host（${e.message}）"
            Log.e(TAG, lastError!!, e)
            return false
        }

        val sock = try {
            DatagramSocket()
        } catch (e: SocketException) {
            lastError = "创建 socket 失败：${e.message}"
            Log.e(TAG, lastError!!, e)
            return false
        }

        try {
            sock.broadcast = false
            sock.reuseAddress = true
            // 低延迟 DSCP（0x10 = IPTOS_LOWDELAY）：局域网 Wi-Fi 上能略微改善排队行为。
            // 个别 ROM 上设置会抛异常，因此包在 try 里，失败不影响发送。
            sock.trafficClass = 0x10
        } catch (e: SocketException) {
            Log.w(TAG, "设置 socket 选项失败（忽略）：${e.message}")
        }

        socket = sock
        targetAddress = address
        targetPort = port
        packetsSent = 0L
        bytesSent = 0L
        Log.i(TAG, "已打开 UDP -> ${address.hostAddress}:$port")
        return true
    }

    /**
     * 发送一个包。
     *
     * @param data   封好的完整报文（32 字节头 + payload）
     * @param length 有效长度（<= data.size）
     * @return true 发送成功
     */
    fun send(data: ByteArray, length: Int): Boolean {
        val sock = socket
        val address = targetAddress
        if (sock == null || address == null) {
            lastError = "socket 未打开"
            return false
        }
        if (length <= 0 || length > data.size) {
            lastError = "发送长度非法: $length / ${data.size}"
            return false
        }
        val packet = DatagramPacket(data, length, address, targetPort)
        return try {
            sock.send(packet)
            failureStreak = 0
            lastError = null
            packetsSent++
            bytesSent += length.toLong()
            true
        } catch (e: SocketException) {
            // 典型：网络切换 / Wi-Fi 掉线 / socket 被对端 ICMP 不可达打坏
            failureStreak++
            lastError = "发送失败(SocketException)：${e.message}"
            Log.w(TAG, lastError!!)
            false
        } catch (e: IOException) {
            failureStreak++
            lastError = "发送失败(IOException)：${e.message}"
            Log.w(TAG, lastError!!)
            false
        } catch (e: SecurityException) {
            failureStreak++
            lastError = "发送被拒绝(SecurityException)：缺少 INTERNET 权限？"
            Log.e(TAG, lastError!!, e)
            false
        } catch (e: IllegalArgumentException) {
            failureStreak++
            lastError = "发送参数非法：${e.message}"
            Log.e(TAG, lastError!!, e)
            false
        }
    }

    /** 关闭 socket，可重复调用。 */
    @Synchronized
    fun close() {
        val sock = socket
        socket = null
        targetAddress = null
        if (sock != null) {
            try {
                sock.close()
            } catch (t: Throwable) {
                Log.w(TAG, "关闭 socket 异常：${t.message}")
            }
        }
    }

    /** 目标地址的可读形式（UI 展示用）。 */
    fun targetDescription(): String {
        val address = targetAddress
        return if (address == null) "<未连接>" else "${address.hostAddress}:$targetPort"
    }
}
