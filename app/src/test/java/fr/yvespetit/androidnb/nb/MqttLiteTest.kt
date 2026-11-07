package fr.yvespetit.androidnb.nb

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.DataInputStream
import java.net.ServerSocket
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

class MqttLiteTest {
    @Test
    fun remainingLengthEncoding() {
        assertArrayEquals(byteArrayOf(0), MqttLite.remainingLength(0))
        assertArrayEquals(byteArrayOf(127), MqttLite.remainingLength(127))
        assertArrayEquals(byteArrayOf(0x80.toByte(), 1), MqttLite.remainingLength(128))
        assertArrayEquals(byteArrayOf(0xFF.toByte(), 0x7F), MqttLite.remainingLength(16383))
    }

    /** A fake broker: checks CONNECT, answers CONNACK, records packets, sends one retained message. */
    @Test
    fun connectSubscribePublishReceive() {
        val server = ServerSocket(0)
        val packets = LinkedBlockingQueue<Pair<Int, ByteArray>>()
        val broker = thread {
            server.accept().use { s ->
                val input = DataInputStream(s.getInputStream())
                val out = s.getOutputStream()
                fun read(): Pair<Int, ByteArray>? {
                    val h = input.read()
                    if (h < 0) return null
                    var len = 0
                    var mult = 1
                    while (true) {
                        val b = input.readUnsignedByte()
                        len += (b and 0x7F) * mult
                        if (b and 0x80 == 0) break
                        mult *= 128
                    }
                    return h to ByteArray(len).also { input.readFully(it) }
                }
                val connect = read()!!
                packets.put(connect)
                out.write(byteArrayOf(0x20, 2, 0, 0))
                while (true) {
                    val p = read() ?: break
                    packets.put(p)
                    if (p.first == 0x82) {
                        out.write(byteArrayOf(0x90.toByte(), 3, p.second[0], p.second[1], 0))
                        val topic = "state/nb/mode".toByteArray()
                        val body = byteArrayOf(0, topic.size.toByte()) + topic + "datv".toByteArray()
                        out.write(byteArrayOf(0x31, body.size.toByte()) + body)
                    }
                    if (p.first == 0xC0) out.write(byteArrayOf(0xD0.toByte(), 0))
                    if (p.first == 0xE0) break
                }
            }
        }
        val m = MqttLite.connect("127.0.0.1", server.localPort, clientId = "test")
        val connect = packets.poll(2, TimeUnit.SECONDS)!!
        assertEquals(0x10, connect.first)
        assertEquals("MQTT", String(connect.second, 2, 4))
        m.subscribe("state/nb/mode")
        m.publish("cmd/nb/mode", "nb")
        val msg = m.receive()!!
        assertEquals("state/nb/mode", msg.topic)
        assertEquals("datv", msg.payload)
        m.setReadTimeout(200)
        m.ping()
        assertNull(m.receive())  // PINGRESP is skipped, then the timeout
        assertEquals(0x82, packets.poll(2, TimeUnit.SECONDS)!!.first)
        val pub = packets.poll(2, TimeUnit.SECONDS)!!
        assertEquals(0x30, pub.first)
        assertEquals("cmd/nb/mode", String(pub.second, 2, 11))
        assertEquals("nb", String(pub.second, 13, 2))
        assertEquals(0xC0, packets.poll(2, TimeUnit.SECONDS)!!.first)
        m.close()
        broker.join(2000)
        server.close()
    }
}
