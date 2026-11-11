package fr.yvespetit.androidnb.nb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class JitterBufferTest {
    private fun pkt(seq: Int) = NbProtocol.AudioPacket(seq and 0xFFFF, byteArrayOf(seq.toByte()))

    @Test
    fun waitsForPrefillThenPlaysInOrder() {
        val b = JitterBuffer(prefill = 3, maxPackets = 10)
        b.put(pkt(0))
        b.put(pkt(1))
        assertNull(b.take(0))
        b.put(pkt(2))
        assertEquals(0, b.take(0)!!.seq)
        assertEquals(1, b.take(0)!!.seq)
        assertEquals(2, b.take(0)!!.seq)
    }

    @Test
    fun underrunRestartsPrefill() {
        val b = JitterBuffer(prefill = 2, maxPackets = 10)
        b.put(pkt(0)); b.put(pkt(1))
        b.take(0); b.take(0)
        assertNull(b.take(0))
        assertEquals(1L, b.underruns)
        b.put(pkt(2))
        assertNull(b.take(0))
        b.put(pkt(3))
        assertNotNull(b.take(0))
    }

    @Test
    fun countsLossAcrossWrap() {
        val b = JitterBuffer(prefill = 1, maxPackets = 10)
        b.put(pkt(65534))
        b.put(pkt(1))  // 65535 and 0 missing
        assertEquals(2L, b.lost)
    }

    @Test
    fun capsDelayByDroppingOldest() {
        val b = JitterBuffer(prefill = 2, maxPackets = 5)
        for (i in 0 until 12) b.put(pkt(i))
        assertEquals(5, b.size)
        assertEquals(7L, b.discarded)
        assertEquals(7, b.take(0)!!.seq)
    }

    @Test
    fun usableAgainAfterReleaseAndClear() {
        val b = JitterBuffer(prefill = 1, maxPackets = 5)
        b.release()
        b.put(pkt(0))
        assertNull(b.take(0))
        b.clear()
        b.put(pkt(1))
        assertEquals(1, b.take(0)!!.seq)
    }
}
