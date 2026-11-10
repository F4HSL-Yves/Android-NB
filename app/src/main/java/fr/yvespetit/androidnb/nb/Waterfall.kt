package fr.yvespetit.androidnb.nb

import kotlin.math.roundToInt

/**
 * Waterfall image and spectrum trace built from nbd's lines, in plain
 * Kotlin (pixels as ARGB ints) so it is testable on the JVM.
 *
 * Rows form a ring: the newest row is at [head], older rows follow, wrapping
 * at [rows]. A change of view (zoom, pan, bins) resamples the old rows onto
 * the new frequency axis, so the history stays in place while new rows come
 * in (parts that were outside the old view are black).
 *
 * Colours follow the noise floor automatically: the floor (20th percentile
 * of a row, smoothed) is dark blue, and [rangeDb] above it is white.
 */
class Waterfall(val rows: Int = 240) {
    var width = 0
        private set
    var pixels = IntArray(0)
        private set
    var head = 0
        private set
    var filled = 0
        private set
    /** Smoothed spectrum of the latest rows, dBFS per bin. */
    var trace = FloatArray(0)
        private set
    /** Absolute view of the current rows (Hz). */
    var leftHz = 0L
        private set
    var spanHz = 0
        private set
    var floorDb = Float.NaN
        private set
    var rangeDb = 45f
    var lines = 0L
        private set

    private var viewKey = Triple(0, 0, 0)

    fun clear() {
        pixels.fill(0)
        filled = 0
        floorDb = Float.NaN
        trace = FloatArray(width) { Float.NaN }
    }

    fun add(line: NbProtocol.SpectrumLine, bandCenterHz: Long) {
        val key = Triple(line.centerOffsetHz, line.spanHz, line.bins)
        if (key != viewKey || line.bins != width) {
            viewKey = key
            val newLeft = bandCenterHz + line.centerOffsetHz - line.spanHz / 2
            if (width > 0 && spanHz > 0 && filled > 0) {
                remap(newLeft, line.spanHz, line.bins)
            } else {
                width = line.bins
                pixels = IntArray(width * rows)
                head = 0
                spanHz = line.spanHz
                leftHz = newLeft
                clear()
            }
        }
        val db = FloatArray(width) { if (line.hasData(it)) line.db(it) else Float.NaN }
        val sorted = db.filter { !it.isNaN() }.sorted()
        if (sorted.isNotEmpty()) {
            val f = sorted[sorted.size / 5]
            floorDb = if (floorDb.isNaN()) f else floorDb * 0.85f + f * 0.15f
        }
        head = (head - 1 + rows) % rows
        if (filled < rows) filled++
        val base = head * width
        val lo = floorDb - 3f
        for (k in 0 until width) {
            val d = db[k]
            pixels[base + k] = if (d.isNaN() || floorDb.isNaN()) PALETTE[0] else {
                val i = ((d - lo) / rangeDb * 255f).roundToInt().coerceIn(0, 255)
                PALETTE[i]
            }
            val t = trace[k]
            trace[k] = if (t.isNaN() || d.isNaN()) d else t * 0.5f + d * 0.5f
        }
        lines++
    }

    /** Moves the image and trace onto a new view: column k takes the old column at the same frequency. */
    private fun remap(newLeft: Long, newSpan: Int, newWidth: Int) {
        val src = IntArray(newWidth) { k ->
            val f = newLeft + (k + 0.5) * newSpan / newWidth
            val o = Math.floor((f - leftHz) / spanHz * width).toInt()
            if (o in 0 until width) o else -1
        }
        val old = pixels
        val oldW = width
        pixels = IntArray(newWidth * rows)
        for (r in 0 until rows) {
            val ob = r * oldW
            val nb = r * newWidth
            for (k in 0 until newWidth) {
                val o = src[k]
                pixels[nb + k] = if (o < 0) PALETTE[0] else old[ob + o]
            }
        }
        val oldTrace = trace
        trace = FloatArray(newWidth) { k -> src[k].let { if (it < 0) Float.NaN else oldTrace[it] } }
        width = newWidth
        leftHz = newLeft
        spanHz = newSpan
    }

    /** Frequency (Hz) at a horizontal fraction 0..1 of the view. */
    fun freqAt(fraction: Float): Long = leftHz + (fraction.toDouble() * spanHz).roundToLong()

    /** Horizontal fraction of the view for a frequency (may fall outside 0..1). */
    fun fractionOf(hz: Long): Float = if (spanHz == 0) 0f else ((hz - leftHz).toDouble() / spanHz).toFloat()

    private fun Double.roundToLong(): Long = kotlin.math.round(this).toLong()

    companion object {
        /** Black, blue, cyan, yellow, red, white: the usual SDR waterfall. */
        val PALETTE: IntArray = run {
            val stops = listOf(
                0.00f to 0x000000, 0.20f to 0x000080, 0.40f to 0x0080FF, 0.60f to 0x00FFFF,
                0.75f to 0xFFFF00, 0.90f to 0xFF0000, 1.00f to 0xFFFFFF,
            )
            IntArray(256) { i ->
                val x = i / 255f
                val hi = stops.indexOfFirst { it.first >= x }.coerceAtLeast(1)
                val (x0, c0) = stops[hi - 1]
                val (x1, c1) = stops[hi]
                val t = if (x1 > x0) (x - x0) / (x1 - x0) else 0f
                fun ch(shift: Int): Int {
                    val a = (c0 shr shift) and 0xFF
                    val b = (c1 shr shift) and 0xFF
                    return (a + (b - a) * t).roundToInt().coerceIn(0, 255)
                }
                (0xFF shl 24) or (ch(16) shl 16) or (ch(8) shl 8) or ch(0)
            }
        }
    }
}
