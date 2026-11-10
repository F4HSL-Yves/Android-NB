package fr.yvespetit.androidnb

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import fr.yvespetit.androidnb.nb.Waterfall
import kotlin.math.roundToInt

private val beacons = listOf(
    Qo100.CW_BEACON_HZ to "CW",
    Qo100.PSK_BEACON_HZ to "PSK",
    Qo100.UPPER_BEACON_HZ to "Balise",
)

/**
 * Spectrum trace over the waterfall, with the receive passband, the dial
 * and the beacons. A tap centres the passband on the tapped signal; a
 * horizontal drag tunes like a knob; a two-finger pinch zooms the view
 * (onZoom gets the scale factor, > 1 when the fingers spread).
 */
@Composable
fun BandView(
    waterfall: Waterfall,
    version: Int,
    dialHz: Long,
    filterLowHz: Int,
    filterHighHz: Int,
    onTune: (Long) -> Unit,
    onZoom: (Float) -> Unit,
    viewLeftHz: Long = 0L,
    viewSpanHz: Int = 0,
    modifier: Modifier = Modifier,
) {
    // The requested view (follows the fingers at once); the waterfall rows
    // are drawn where they belong in it until the board sends the new view.
    val view = if (viewSpanHz > 0) BandAxis(viewLeftHz, viewSpanHz) else BandAxis(waterfall.leftHz, waterfall.spanHz)
    val viewNow by rememberUpdatedState(view)
    val tune by rememberUpdatedState(onTune)
    val zoom by rememberUpdatedState(onZoom)
    val dial by rememberUpdatedState(dialHz)
    val passbandMid = (filterLowHz + filterHighHz) / 2
    val holder = remember { BitmapHolder() }
    val drag = remember { DoubleArray(1) }  // dial during a drag, Hz
    Box(
        modifier = modifier.fillMaxWidth().height(300.dp).background(Color.Black)
            .pointerInput(waterfall) {
                detectTapGestures { p ->
                    if (viewNow.spanHz > 0) {
                        val f = viewNow.freqAt(p.x / size.width) - passbandMid
                        tune((f / 100.0).roundToInt() * 100L)
                    }
                }
            }
            .pointerInput(waterfall) {
                // One finger: horizontal drag tunes. Two fingers: pinch zooms.
                awaitEachGesture {
                    val first = awaitFirstDown(requireUnconsumed = false)
                    var dragging = false
                    var pinching = false
                    var travel = 0f
                    while (true) {
                        val event = awaitPointerEvent()
                        if (event.changes.none { it.pressed }) break
                        if (event.changes.count { it.pressed } >= 2) {
                            pinching = true
                            val z = event.calculateZoom()
                            if (z != 1f && z.isFinite()) zoom(z)
                            event.changes.forEach { it.consume() }
                            continue
                        }
                        if (pinching) {  // one finger left after a pinch: ignore it
                            event.changes.forEach { it.consume() }
                            continue
                        }
                        val ch = event.changes.firstOrNull { it.id == first.id } ?: break
                        if (!dragging && ch.isConsumed) break  // the page scrolled
                        val dx = ch.positionChange().x
                        if (!dragging) {
                            travel += dx
                            if (kotlin.math.abs(travel) < viewConfiguration.touchSlop) continue
                            dragging = true
                            drag[0] = dial.toDouble()
                        }
                        ch.consume()
                        val vs = viewNow.spanHz
                        if (vs > 0 && dx != 0f) {
                            drag[0] -= dx.toDouble() / size.width * vs
                            val step = if (vs > 100_000) 100.0 else 10.0
                            tune(((drag[0] / step).roundToInt() * step).toLong())
                        }
                    }
                }
            },
    ) {
        Canvas(modifier = Modifier.fillMaxWidth().height(300.dp)) {
            if (version < 0) return@Canvas  // reading version redraws on each new row
            val traceH = size.height * 0.3f
            val x0 = view.fractionOf(waterfall.leftHz) * size.width
            val x1 = view.fractionOf(waterfall.leftHz + waterfall.spanHz) * size.width
            drawRect(Color(0xFF0A0F1A), size = Size(size.width, traceH))
            clipRect {
                drawWaterfall(waterfall, holder, traceH, x0, x1)
                drawTrace(waterfall, traceH, x0, x1)
            }
            drawMarkers(view, dialHz, filterLowHz, filterHighHz)
        }
    }
}

/** Horizontal frequency axis of the display. */
private class BandAxis(val leftHz: Long, val spanHz: Int) {
    fun freqAt(fraction: Float): Long = leftHz + kotlin.math.round(fraction.toDouble() * spanHz).toLong()
    fun fractionOf(hz: Long): Float = if (spanHz == 0) 0f else ((hz - leftHz).toDouble() / spanHz).toFloat()
}

private class BitmapHolder {
    var bitmap: Bitmap? = null
}

private fun DrawScope.drawWaterfall(w: Waterfall, holder: BitmapHolder, top: Float, x0: Float, x1: Float) {
    if (w.width == 0 || w.filled == 0) return
    var bmp = holder.bitmap
    if (bmp == null || bmp.width != w.width || bmp.height != w.rows) {
        bmp = Bitmap.createBitmap(w.width, w.rows, Bitmap.Config.ARGB_8888)
        holder.bitmap = bmp
    }
    bmp!!.setPixels(w.pixels, 0, w.width, 0, 0, w.width, w.rows)
    val img = bmp.asImageBitmap()
    val rowH = (size.height - top) / w.rows
    // Newest rows (head .. rows-1) first, then the wrapped part (0 .. head-1).
    val first = minOf(w.rows - w.head, w.filled)
    drawImage(
        img, srcOffset = IntOffset(0, w.head), srcSize = IntSize(w.width, first),
        dstOffset = IntOffset(x0.roundToInt(), top.roundToInt()), dstSize = IntSize((x1 - x0).roundToInt().coerceAtLeast(1), (first * rowH).roundToInt()),
    )
    val rest = w.filled - first
    if (rest > 0) {
        drawImage(
            img, srcOffset = IntOffset(0, 0), srcSize = IntSize(w.width, rest),
            dstOffset = IntOffset(x0.roundToInt(), (top + first * rowH).roundToInt()),
            dstSize = IntSize((x1 - x0).roundToInt().coerceAtLeast(1), (rest * rowH).roundToInt()),
        )
    }
}

private fun DrawScope.drawTrace(w: Waterfall, h: Float, x0: Float, x1: Float) {
    val n = w.trace.size
    if (n < 2 || w.floorDb.isNaN()) return
    val lo = w.floorDb - 5f
    val range = w.rangeDb + 15f
    val path = Path()
    var started = false
    for (k in 0 until n) {
        val d = w.trace[k]
        if (d.isNaN()) continue
        val x = x0 + (k + 0.5f) / n * (x1 - x0)
        val y = h - ((d - lo) / range).coerceIn(0f, 1f) * h
        if (!started) path.moveTo(x, y) else path.lineTo(x, y)
        started = true
    }
    drawPath(path, Color(0xFF80D8FF), style = Stroke(width = 1.5f))
}

private fun DrawScope.drawMarkers(w: BandAxis, dialHz: Long, low: Int, high: Int) {
    if (w.spanHz == 0) return
    val x0 = w.fractionOf(dialHz + low) * size.width
    val x1 = w.fractionOf(dialHz + high) * size.width
    drawRect(Color(0x33FFFFFF), topLeft = Offset(x0, 0f), size = Size(maxOf(2f, x1 - x0), size.height))
    val xd = w.fractionOf(dialHz) * size.width
    drawLine(Color(0xFFFF5252), Offset(xd, 0f), Offset(xd, size.height), strokeWidth = 2f)

    val paint = android.graphics.Paint().apply {
        color = android.graphics.Color.WHITE
        textSize = 11.dp.toPx()
        isAntiAlias = true
    }
    // Frequency scale: kHz above 10489 MHz, at a round step for the span.
    val stepHz = listOf(1_000L, 2_000L, 5_000L, 10_000L, 25_000L, 50_000L, 100_000L)
        .firstOrNull { w.spanHz / it <= 6 } ?: 100_000L
    var f = (w.leftHz / stepHz + 1) * stepHz
    while (f < w.leftHz + w.spanHz) {
        val x = w.fractionOf(f) * size.width
        drawLine(Color(0x66FFFFFF), Offset(x, 0f), Offset(x, 6.dp.toPx()), strokeWidth = 1f)
        val label = "%03d".format((f / 1000) % 1000)
        drawIntoCanvas { it.nativeCanvas.drawText(label, x + 3f, 16.dp.toPx(), paint) }
        f += stepHz
    }
    paint.color = android.graphics.Color.rgb(255, 213, 79)
    for ((hz, name) in beacons) {
        val fx = w.fractionOf(hz)
        if (fx !in 0f..1f) continue
        val x = fx * size.width
        val tri = Path().apply {
            moveTo(x, 20.dp.toPx())
            lineTo(x - 5.dp.toPx(), 12.dp.toPx())
            lineTo(x + 5.dp.toPx(), 12.dp.toPx())
            close()
        }
        drawPath(tri, Color(0xFFFFD54F))
        drawIntoCanvas { it.nativeCanvas.drawText(name, x + 6.dp.toPx(), 30.dp.toPx(), paint) }
    }
}
