package fr.yvespetit.androidnb

import kotlin.math.floor

/**
 * S-meter scale: 6 dB per S unit up to S9, then dB over S9 (S9+10, +20…).
 *
 * nbd's signal level is relative, not calibrated in dBm (a quiet band reads
 * about -59 dB on F4HSL's board): by default S9 is put at -17 dB, so that
 * noise reads S2. Réglages réception moves it, or sets it from the noise.
 * Same scale as the iOS app.
 */
object SMeterScale {
    const val DEFAULT_S9_DB = -17.0
    const val DB_PER_S = 6.0
    /** The bar ends at S9+60. */
    const val OVER_S9_MAX = 60.0
    /** Share of the bar for S0…S9; the rest is S9 to S9+60. */
    const val S9_FRACTION = 0.6

    /** S units, fractional (9 at S9, more above). */
    fun sUnits(db: Double, s9: Double = DEFAULT_S9_DB): Double = 9 + (db - s9) / DB_PER_S

    /** "S0"…"S9", then "S9+10", "S9+20"… (whole tens of dB over S9). */
    fun label(db: Double, s9: Double = DEFAULT_S9_DB): String {
        val over = db - s9
        if (over >= 10) return "S9+${(over / 10).toInt() * 10}"
        if (over >= 0) return "S9"
        return "S${floor(sUnits(db, s9)).toInt().coerceAtLeast(0)}"
    }

    /** Bar position, 0…1: S0…S9 on the first [S9_FRACTION], S9…S9+60 after. */
    fun fraction(db: Double, s9: Double = DEFAULT_S9_DB): Double {
        val over = db - s9
        if (over <= 0) return sUnits(db, s9).coerceAtLeast(0.0) / 9 * S9_FRACTION
        return S9_FRACTION + (1 - S9_FRACTION) * (over / OVER_S9_MAX).coerceAtMost(1.0)
    }

    /** S9 level that puts [noiseDb] at S2. */
    fun s9ForNoiseAtS2(noiseDb: Double): Double = floor(noiseDb + 7 * DB_PER_S)

    /** Tick marks of the scale: bar position and label. */
    val ticks: List<Pair<Double, String>> =
        listOf(1, 3, 5, 7, 9).map { it / 9.0 * S9_FRACTION to "$it" } +
            listOf(20, 40, 60).map { S9_FRACTION + (1 - S9_FRACTION) * it / OVER_S9_MAX to "+$it" }
}
