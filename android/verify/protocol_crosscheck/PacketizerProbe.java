/**
 * PacketizerProbe -- Packetizer.kt 的 Java 逻辑镜像，用于**跨平台校验封包字节布局**。
 *
 * 为什么需要它：
 *   本仓库的 Linux 沙箱没有 Android SDK/NDK，无法编译 Kotlin。
 *   但"Android 端封出来的包，服务端能不能解"是整条链路最关键的一致性要求。
 *   这里把 Packetizer.kt 的 pack() / getBe16() / getBe32() **逐行等价**地用 Java 复刻一遍，
 *   再用真实的 shared/core/src/protocol.c 编出参考包，两者做逐字节比对。
 *
 * 注意（诚实声明）：
 *   它校验的是**逻辑**（常量、偏移、字节序），不是 Kotlin 源码本身。
 *   如果有人改了 Packetizer.kt 却忘了同步这里，本检查不会发现——
 *   因此修改 Packetizer.kt 时必须同步修改本文件，见同目录 README.md。
 */
public class PacketizerProbe {

    // ---- 必须 Packetizer.kt 与 womic_protocol.h 三方一致 ----
    static final int MAGIC = 0x574F4D50;       // WOMIC_PROTOCOL_MAGIC 'WOMP'
    static final int VERSION = 1;              // WOMIC_PROTOCOL_VERSION
    static final int HEADER_SIZE = 32;         // WOMIC_PACKET_HEADER_SIZE
    static final int MAX_PAYLOAD = 4096;       // WOMIC_PROTO_MAX_PAYLOAD
    static final int FLAG_FEC = 0x0001;        // WOMIC_FLAG_FEC
    static final int FLAG_DTX = 0x0002;        // WOMIC_FLAG_DTX

    static void putBe16(byte[] dst, int off, int v) {
        dst[off] = (byte) (v >>> 8);
        dst[off + 1] = (byte) v;
    }

    static void putBe32(byte[] dst, int off, int v) {
        dst[off] = (byte) (v >>> 24);
        dst[off + 1] = (byte) (v >>> 16);
        dst[off + 2] = (byte) (v >>> 8);
        dst[off + 3] = (byte) v;
    }

    static int getBe16(byte[] s, int off) {
        return ((s[off] & 0xFF) << 8) | (s[off + 1] & 0xFF);
    }

    static int getBe32(byte[] s, int off) {
        return ((s[off] & 0xFF) << 24) | ((s[off + 1] & 0xFF) << 16)
                | ((s[off + 2] & 0xFF) << 8) | (s[off + 3] & 0xFF);
    }

    /** 与 Packetizer.pack() 逐行等价。 */
    static int pack(byte[] dst, int seq, long timestampMs, int sampleRate, int channels,
                    int frames, byte[] payload, int payloadLen, int flags) {
        if (payloadLen < 0 || payloadLen > MAX_PAYLOAD) {
            throw new IllegalArgumentException("payloadLen 越界: " + payloadLen);
        }
        if (dst.length < HEADER_SIZE + payloadLen) {
            throw new IllegalArgumentException("目标缓冲过小");
        }
        dst[0] = (byte) (MAGIC >>> 24);
        dst[1] = (byte) (MAGIC >>> 16);
        dst[2] = (byte) (MAGIC >>> 8);
        dst[3] = (byte) MAGIC;
        putBe16(dst, 4, VERSION);
        putBe16(dst, 6, HEADER_SIZE);
        putBe32(dst, 8, seq);
        putBe32(dst, 12, (int) (timestampMs & 0xFFFFFFFFL));
        putBe32(dst, 16, sampleRate);
        putBe16(dst, 20, channels);
        putBe16(dst, 22, frames);
        putBe16(dst, 24, payloadLen);
        putBe16(dst, 26, flags);
        putBe32(dst, 28, 0);
        System.arraycopy(payload, 0, dst, HEADER_SIZE, payloadLen);
        return HEADER_SIZE + payloadLen;
    }

    /** 与 Packetizer.selfCheck() 等价的自洽校验。 */
    static String selfCheck(byte[] packet) {
        if (packet.length < HEADER_SIZE) {
            return "包长 " + packet.length + " < 头部 " + HEADER_SIZE;
        }
        if (getBe32(packet, 0) != MAGIC) {
            return "magic 不匹配";
        }
        if (getBe16(packet, 4) != VERSION) {
            return "version 不匹配";
        }
        int headerSize = getBe16(packet, 6);
        if (headerSize < HEADER_SIZE || headerSize > 256) {
            return "header_size 非法: " + headerSize;
        }
        int payloadBytes = getBe16(packet, 24);
        if (payloadBytes > MAX_PAYLOAD) {
            return "payload_bytes 越界: " + payloadBytes;
        }
        if (packet.length != headerSize + payloadBytes) {
            return "长度不自洽: " + packet.length + " != " + headerSize + " + " + payloadBytes;
        }
        return null;
    }

    public static void main(String[] args) {
        // 与 run.sh 里 C 参考程序（pktc.c）完全相同的输入
        byte[] payload = new byte[] {(byte) 0xDE, (byte) 0xAD, (byte) 0xBE, (byte) 0xEF, 0x01};
        byte[] dst = new byte[128];
        int n = pack(dst, 0x12345678, 0x01020304L, 48000, 1, 960, payload, 5, FLAG_FEC);

        System.out.println("len=" + n);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            sb.append(String.format("%02X", dst[i]));
        }
        System.out.println(sb.toString());

        // 服务端要求：报文长度必须恰好 = header_size + payload_bytes，
        // 因此这里按真实包长截出来再做自洽校验（实际发送时也是这么截的）。
        byte[] packet = java.util.Arrays.copyOf(dst, n);

        String err = selfCheck(packet);
        if (err != null) {
            System.out.println("SELFCHECK-FAIL: " + err);
            System.exit(1);
        }
        // 再解一遍字段，确认与写入值一致
        if (getBe32(packet, 8) != 0x12345678 || getBe32(packet, 16) != 48000
                || getBe16(packet, 20) != 1 || getBe16(packet, 22) != 960
                || getBe16(packet, 24) != 5 || getBe16(packet, 26) != FLAG_FEC) {
            System.out.println("FIELD-MISMATCH");
            System.exit(1);
        }
        System.out.println("SELFFIELDS-OK seq=" + getBe32(packet, 8) + " rate=" + getBe32(packet, 16)
                + " frames=" + getBe16(packet, 22) + " payload=" + getBe16(packet, 24)
                + " flags=" + getBe16(packet, 26));
    }
}
