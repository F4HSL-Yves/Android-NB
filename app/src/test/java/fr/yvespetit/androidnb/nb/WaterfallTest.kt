package fr.yvespetit.androidnb.nb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class WaterfallTest {
    private fun line(center: Int, span: Int, values: IntArray, seq: Int = 0) =
        NbProtocol.SpectrumLine(seq, center, span, -100f, 0.5f, ByteArray(values.size) { values[it].toByte() })

    @Test
    fun rowsRingNewestFirst() {
        val w = Waterfall(rows = 4)
        val flat = IntArray(100) { 20 }
        repeat(3) { w.add(line(0, 768000, flat), 10_489_750_000L) }
        assertEquals(100, w.width)
        assertEquals(3, w.filled)
        assertEquals(1, w.head)  // 0 -> 3 -> 2 -> 1
        repeat(3) { w.add(line(0, 768000, flat), 10_489_750_000L) }
        assertEquals(4, w.filled)
        assertEquals(10_489_750_000L - 384_000L, w.leftHz)
    }

    @Test
    fun strongBinIsBrighterThanFloorAndTraced() {
        val w = Waterfall(rows = 8)
        val v = IntArray(64) { 20 }  // -90 dB floor
        v[40] = 100                  // -50 dB carrier
        repeat(5) { w.add(line(0, 768000, v), 10_489_750_000L) }
        assertEquals(-90f, w.floorDb, 0.01f)
        val row = w.head * w.width
        assertNotEquals(w.pixels[row + 10], w.pixels[row + 40])
        assertTrue(brightness(w.pixels[row + 40]) > brightness(w.pixels[row + 10]))
        assertEquals(-50f, w.trace[40], 0.01f)
    }

    @Test
    fun viewChangeKeepsHistory() {
        val w = Waterfall(rows = 8)
        repeat(3) { w.add(line(0, 768000, IntArray(32) { 20 }), 10_489_750_000L) }
        w.add(line(120000, 24000, IntArray(32) { 20 }), 10_489_750_000L)
        assertEquals(4, w.filled)
        assertEquals(10_489_750_000L + 120_000L - 12_000L, w.leftHz)
        assertEquals(10_489_870_000L, w.freqAt(0.5f))
        assertEquals(0.5f, w.fractionOf(10_489_870_000L), 1e-6f)
    }

    @Test
    fun zoomResamplesOldRows() {
        val w = Waterfall(rows = 8)
        val v = IntArray(64) { 20 }
        v[40] = 100  // carrier at centre + 102 kHz
        repeat(3) { w.add(line(0, 768000, v), 10_489_750_000L) }
        val bright = w.pixels[w.head * w.width + 40]
        // Zoom to 24 kHz around the carrier: the old rows keep it under column 16.
        w.add(line(102000, 24000, IntArray(32) { 20 }), 10_489_750_000L)
        val oldRow = ((w.head + 1) % w.rows) * w.width
        assertEquals(32, w.width)
        assertEquals(bright, w.pixels[oldRow + 16])
        assertNotEquals(bright, w.pixels[oldRow + 2])
    }

    private fun brightness(argb: Int) = ((argb shr 16) and 0xFF) + ((argb shr 8) and 0xFF) + (argb and 0xFF)
}
