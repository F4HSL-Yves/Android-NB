package fr.yvespetit.androidnb.nb

import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64

/**
 * Minimal WebSocket client (RFC 6455) on a plain socket: enough for nbd, no
 * library. [receive] blocks on the caller's thread; [sendText] may be called
 * from any thread.
 */
class NbWebSocket private constructor(private val socket: Socket) {
    private val input = DataInputStream(BufferedInputStream(socket.getInputStream(), 65536))
    private val output: OutputStream = socket.getOutputStream()
    private val random = SecureRandom()
    private val sendLock = Any()

    sealed interface Frame
    class Text(val text: String) : Frame
    class Binary(val data: ByteArray) : Frame
    object Closed : Frame

    companion object {
        private const val GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        private const val MAX_FRAME = 1 shl 20

        fun acceptKey(clientKey: String): String =
            Base64.getEncoder().encodeToString(
                MessageDigest.getInstance("SHA-1").digest((clientKey + GUID).toByteArray(Charsets.US_ASCII)),
            )

        /** ws://host:port/path; throws IOException when nbd is unreachable or refuses. */
        fun connect(url: String, timeoutMs: Int = 5000): NbWebSocket {
            val uri = URI(url)
            require(uri.scheme == "ws") { "only ws:// is supported" }
            val host = uri.host ?: throw IOException("no host in $url")
            val port = if (uri.port > 0) uri.port else 80
            val socket = Socket()
            try {
                socket.connect(InetSocketAddress(host, port), timeoutMs)
                socket.tcpNoDelay = true
                socket.soTimeout = timeoutMs
                val keyBytes = ByteArray(16).also { SecureRandom().nextBytes(it) }
                val key = Base64.getEncoder().encodeToString(keyBytes)
                val path = uri.rawPath.takeUnless { it.isNullOrEmpty() } ?: "/"
                val request = "GET $path HTTP/1.1\r\nHost: $host:$port\r\nUpgrade: websocket\r\n" +
                    "Connection: Upgrade\r\nSec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n"
                socket.getOutputStream().apply {
                    write(request.toByteArray(Charsets.US_ASCII))
                    flush()
                }
                val ws = NbWebSocket(socket)
                val head = ws.readHead()
                val status = head.lineSequence().firstOrNull().orEmpty()
                if (!status.contains(" 101 ")) throw IOException("nbd refused the WebSocket: $status")
                val accept = head.lineSequence()
                    .firstOrNull { it.startsWith("sec-websocket-accept:", ignoreCase = true) }
                    ?.substringAfter(':')?.trim()
                if (accept != acceptKey(key)) throw IOException("bad Sec-WebSocket-Accept")
                // Status arrives every second: 15 s of silence means the link is dead.
                socket.soTimeout = 15000
                return ws
            } catch (e: Exception) {
                socket.close()
                throw e
            }
        }
    }

    private fun readHead(): String {
        val sb = StringBuilder()
        while (!sb.endsWith("\r\n\r\n")) {
            val b = input.read()
            if (b < 0) throw IOException("connection closed during handshake")
            sb.append(b.toChar())
            if (sb.length > 8192) throw IOException("handshake too long")
        }
        return sb.toString()
    }

    /** Next text or binary message. Answers pings itself. */
    fun receive(): Frame {
        var message: java.io.ByteArrayOutputStream? = null
        var messageOpcode = 0
        while (true) {
            val b0 = input.read()
            if (b0 < 0) return Closed
            val b1 = input.readUnsignedByte()
            val fin = b0 and 0x80 != 0
            val opcode = b0 and 0x0F
            var len = (b1 and 0x7F).toLong()
            if (len == 126L) len = input.readUnsignedShort().toLong()
            else if (len == 127L) len = input.readLong()
            if (len < 0 || len > MAX_FRAME) throw IOException("frame too large")
            val mask = if (b1 and 0x80 != 0) ByteArray(4).also { input.readFully(it) } else null
            val payload = ByteArray(len.toInt())
            input.readFully(payload)
            if (mask != null) for (i in payload.indices) payload[i] = (payload[i].toInt() xor mask[i % 4].toInt()).toByte()
            when (opcode) {
                0x8 -> {
                    runCatching { sendFrame(0x8, payload.copyOf(minOf(2, payload.size))) }
                    return Closed
                }
                0x9 -> sendFrame(0xA, payload)
                0xA -> {}
                0x1, 0x2 -> {
                    if (fin) return if (opcode == 0x1) Text(String(payload, Charsets.UTF_8)) else Binary(payload)
                    messageOpcode = opcode
                    message = java.io.ByteArrayOutputStream().apply { write(payload) }
                }
                0x0 -> {
                    val m = message ?: throw IOException("unexpected continuation frame")
                    m.write(payload)
                    if (m.size() > MAX_FRAME) throw IOException("message too large")
                    if (fin) {
                        val data = m.toByteArray()
                        return if (messageOpcode == 0x1) Text(String(data, Charsets.UTF_8)) else Binary(data)
                    }
                }
                else -> throw IOException("unknown opcode $opcode")
            }
        }
    }

    fun sendText(text: String) = sendFrame(0x1, text.toByteArray(Charsets.UTF_8))

    private fun sendFrame(opcode: Int, payload: ByteArray) {
        val mask = ByteArray(4).also { random.nextBytes(it) }
        val n = payload.size
        val header = when {
            n < 126 -> byteArrayOf((0x80 or opcode).toByte(), (0x80 or n).toByte())
            n < 65536 -> byteArrayOf((0x80 or opcode).toByte(), (0x80 or 126).toByte(), (n shr 8).toByte(), n.toByte())
            else -> throw IOException("message too large")
        }
        val masked = ByteArray(n) { (payload[it].toInt() xor mask[it % 4].toInt()).toByte() }
        synchronized(sendLock) {
            output.write(header + mask + masked)
            output.flush()
        }
    }

    /** Polite close; also unblocks a thread waiting in [receive]. */
    fun close() {
        runCatching { sendFrame(0x8, byteArrayOf(0x03, 0xE8.toByte())) }
        runCatching { socket.close() }
    }
}
