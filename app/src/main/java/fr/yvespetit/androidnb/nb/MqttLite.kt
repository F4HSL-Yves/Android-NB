package fr.yvespetit.androidnb.nb

import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * Smallest MQTT 3.1.1 client that the app needs, on a plain socket to the
 * board's mosquitto (port 1883, anonymous): QoS 0 publish, subscribe, ping.
 * Used to switch the board between NB (nbd) and DATV (zynqrx) through the
 * board's own nbctl.sh (topics cmd/nb/mode and state/nb/mode).
 */
class MqttLite private constructor(private val socket: Socket) {
    private val input = DataInputStream(BufferedInputStream(socket.getInputStream()))
    private val output: OutputStream = socket.getOutputStream()
    private val sendLock = Any()
    private var packetId = 1

    class Message(val topic: String, val payload: String)

    companion object {
        const val KEEPALIVE_S = 30

        fun connect(host: String, port: Int = 1883, clientId: String, timeoutMs: Int = 5000): MqttLite {
            val socket = Socket()
            try {
                socket.connect(InetSocketAddress(host, port), timeoutMs)
                socket.tcpNoDelay = true
                socket.soTimeout = timeoutMs
                val m = MqttLite(socket)
                val body = str("MQTT") + byteArrayOf(4, 0x02 /* clean session */) +
                    u16(KEEPALIVE_S) + str(clientId)
                m.send(0x10, body)
                val (type, data) = m.readPacket() ?: throw IOException("MQTT: connexion fermée")
                if (type != 0x20 || data.size < 2) throw IOException("MQTT: réponse inattendue")
                if (data[1].toInt() != 0) throw IOException("MQTT: connexion refusée (${data[1]})")
                return m
            } catch (e: Exception) {
                socket.close()
                throw e
            }
        }

        private fun u16(v: Int) = byteArrayOf((v shr 8).toByte(), (v and 0xFF).toByte())
        private fun str(s: String): ByteArray = s.toByteArray(Charsets.UTF_8).let { u16(it.size) + it }

        internal fun remainingLength(n: Int): ByteArray {
            val out = ArrayList<Byte>()
            var x = n
            do {
                var b = x % 128
                x /= 128
                if (x > 0) b = b or 0x80
                out.add(b.toByte())
            } while (x > 0)
            return out.toByteArray()
        }
    }

    fun publish(topic: String, payload: String, retain: Boolean = false) =
        send(0x30 or (if (retain) 1 else 0), str(topic) + payload.toByteArray(Charsets.UTF_8))

    fun subscribe(topic: String) {
        val id = packetId++
        send(0x82, u16(id) + str(topic) + byteArrayOf(0))
    }

    fun ping() = send(0xC0, ByteArray(0))

    /** Sets how long [receive] waits before returning null with the link still up. */
    fun setReadTimeout(ms: Int) {
        socket.soTimeout = ms
    }

    /**
     * Next application message. Returns null when the read timeout expires
     * (the caller pings then); throws IOException when the link is gone.
     */
    fun receive(): Message? {
        while (true) {
            val p = try {
                readPacket()
            } catch (_: SocketTimeoutException) {
                return null
            } ?: throw IOException("MQTT: connexion fermée")
            val (type, data) = p
            if (type and 0xF0 != 0x30) continue  // SUBACK, PINGRESP...
            val len = ((data[0].toInt() and 0xFF) shl 8) or (data[1].toInt() and 0xFF)
            var pos = 2 + len
            if ((type shr 1) and 3 > 0) pos += 2  // QoS 1/2 carry a packet id
            val topic = String(data, 2, len, Charsets.UTF_8)
            return Message(topic, String(data, pos, data.size - pos, Charsets.UTF_8))
        }
    }

    fun close() {
        runCatching { send(0xE0, ByteArray(0)) }
        runCatching { socket.close() }
    }

    private fun send(header: Int, body: ByteArray) {
        val packet = byteArrayOf(header.toByte()) + remainingLength(body.size) + body
        synchronized(sendLock) {
            output.write(packet)
            output.flush()
        }
    }

    private fun readPacket(): Pair<Int, ByteArray>? {
        val first = input.read()
        if (first < 0) return null
        var len = 0
        var mult = 1
        while (true) {
            val b = input.readUnsignedByte()
            len += (b and 0x7F) * mult
            if (b and 0x80 == 0) break
            mult *= 128
            if (mult > 128 * 128 * 128) throw IOException("MQTT: longueur invalide")
        }
        val data = ByteArray(len)
        input.readFully(data)
        return first to data
    }
}
