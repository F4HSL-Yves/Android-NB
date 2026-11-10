package fr.yvespetit.androidnb.nb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NbProtocolTest {
    // Copied from a real nbd run (daemon/tools/nbws_probe.py).
    private val hello = """{"type":"hello","version":1,"center":10489750000,"rate":768000,"min_freq":10489369000,""" +
        """"max_freq":10490128000,"audio":{"codec":"opus","rate":48000,"channels":1,"frame_ms":20},"source":"file"}"""
    private val status = """{"type":"status","freq":10489901000,"beacon":"locked","correction":2501.7,"drift":0.703,""" +
        """"snr":74.4,"s":-46.0,"filter":[200,2800],"agc":3,"bitrate":12000,"clients":1,"dropped":0,"uptime":2}"""

    @Test
    fun hello() {
        val m = NbProtocol.parse(hello) as NbProtocol.HelloMessage
        assertEquals(1, m.hello.version)
        assertEquals(10_489_750_000L, m.hello.centerHz)
        assertEquals(10_489_369_000L, m.hello.minFreqHz)
        assertEquals(10_490_128_000L, m.hello.maxFreqHz)
        assertEquals("file", m.hello.source)
    }

    @Test
    fun status() {
        val s = (NbProtocol.parse(status) as NbProtocol.StatusMessage).status
        assertEquals(10_489_901_000L, s.freqHz)
        assertEquals(NbProtocol.BeaconState.LOCKED, s.beacon)
        assertEquals(2501.7, s.correctionHz, 1e-9)
        assertEquals(-46.0, s.signalDb, 1e-9)
        assertEquals(200, s.filterLowHz)
        assertEquals(2800, s.filterHighHz)
        assertEquals(3, s.agc)
        assertEquals(12000, s.bitrate)
    }

    @Test
    fun error() {
        val m = NbProtocol.parse("""{"type":"error","message":"freq outside the received band"}""")
        assertEquals("freq outside the received band", (m as NbProtocol.ErrorMessage).text)
    }

    @Test
    fun audio() {
        val p = NbProtocol.parseAudio(byteArrayOf(1, 0x12, 0x34, 9, 8))!!
        assertEquals(0x1234, p.seq)
        assertEquals(2, p.opus.size)
        assertNull(NbProtocol.parseAudio(byteArrayOf(2, 0, 0, 1)))
    }

    @Test
    fun commandsAreWhatNbdParses() {
        assertEquals("""{"cmd":"freq","hz":10489900000}""", NbProtocol.freqCommand(10_489_900_000L))
        assertTrue(NbProtocol.filterCommand(300, 2700).contains(""""low":300,"high":2700"""))
    }

    @Test
    fun acceptKeyFromRfc6455() {
        assertEquals("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", NbWebSocket.acceptKey("dGhlIHNhbXBsZSBub25jZQ=="))
    }

    @Test
    fun helloSpectrumSource() {
        val h = (NbProtocol.parse(
            """{"type":"hello","version":1,"center":10489750000,"rate":768000,"min_freq":10489369000,""" +
                """"max_freq":10490128000,"audio":{"codec":"opus"},"source":"pluto","spectrum":{"source":"maia","max_bins":2048}}""",
        ) as NbProtocol.HelloMessage).hello
        assertEquals("pluto", h.source)
        assertEquals("maia", h.spectrumSource)
        val old = (NbProtocol.parse("""{"type":"hello","version":1,"center":1,"source":"file"}""") as NbProtocol.HelloMessage).hello
        assertEquals("off", old.spectrumSource)
    }

    @Test
    fun spectrumLine() {
        // seq 0x0102, centre -120000 Hz, span 24000 Hz, ref -95.5 dB, step 0.5 dB, 3 bins.
        val f = byteArrayOf(
            2, 1, 2,
            0xFF.toByte(), 0xFE.toByte(), 0x2B, 0x40,
            0, 0, 0x5D, 0xC0.toByte(),
            0xFC.toByte(), 0x45,
            5,
            0, 10, 0xFF.toByte(),
        )
        val l = NbProtocol.parseSpectrum(f)!!
        assertEquals(0x0102, l.seq)
        assertEquals(-120000, l.centerOffsetHz)
        assertEquals(24000, l.spanHz)
        assertEquals(3, l.bins)
        assertEquals(-95.5f, l.db(0), 1e-4f)
        assertEquals(-90.5f, l.db(1), 1e-4f)
        assertEquals(32.0f, l.db(2), 1e-4f)
        assertTrue(!l.hasData(0) && l.hasData(1))
        assertNull(NbProtocol.parseAudio(f))
        assertNull(NbProtocol.parseSpectrum(byteArrayOf(1, 0, 0, 1)))
        assertEquals(
            """{"cmd":"spectrum","center":0,"span":520000,"bins":1024,"rate":8.0}""",
            NbProtocol.spectrumCommand(0, 520000, 1024, 8.0),
        )
    }

    @Test
    fun transmitMessages() {
        val hello = NbProtocol.parse(
            """{"type":"hello","version":1,"center":10489750000,"rate":768000,"min_freq":1,"max_freq":2,""" +
                """"audio":{"codec":"opus"},"source":"pluto","spectrum":{"source":"maia","max_bins":2048},"tx":true}""",
        ) as NbProtocol.HelloMessage
        assertTrue(hello.hello.tx)
        assertEquals("maia", hello.hello.spectrumSource)
        val noTx = NbProtocol.parse("""{"type":"hello","version":1,"spectrum":{"source":"off"},"tx":false}""")
        assertTrue(!(noTx as NbProtocol.HelloMessage).hello.tx)

        val m = NbProtocol.parse(
            """{"type":"tx","state":"on","message":"","owner":3,"uplink":2400300000,"lo":2399500000,"cal":-120,""" +
                """"tune":true,"mic":-12.5,"alc":-3.0,"out":-1.2,"underruns":2,"dropped":0,"on_s":4.5}""",
        ) as NbProtocol.TxMessage
        assertEquals("on", m.tx.state)
        assertEquals(2_400_300_000L, m.tx.uplinkHz)
        assertEquals(2_399_500_000L, m.tx.loHz)
        assertEquals(-120.0, m.tx.calHz, 1e-9)
        assertTrue(m.tx.tune)
        assertEquals(-12.5, m.tx.micDb, 1e-9)
        assertEquals(2L, m.tx.underruns)
        val off = NbProtocol.parse("""{"type":"tx","state":"off","message":"phone disconnected","tune":false}""")
        assertEquals("phone disconnected", (off as NbProtocol.TxMessage).tx.message)

        val p = NbProtocol.micPacket(0x1234, byteArrayOf(9, 8))
        assertEquals(listOf<Byte>(3, 0x12, 0x34, 9, 8), p.toList())
        assertEquals("""{"cmd":"tx","on":1}""", NbProtocol.txCommand(true))
        assertEquals("""{"cmd":"tune","on":0,"hz":1000}""", NbProtocol.tuneCommand(false))
        assertEquals("""{"cmd":"txcal","hz":-120}""", NbProtocol.txCalCommand(-120))
    }
}
