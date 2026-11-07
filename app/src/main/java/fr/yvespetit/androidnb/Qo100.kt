package fr.yvespetit.androidnb

import java.util.Locale

/** QO-100 narrowband transponder plan and frequency helpers. All values in Hz. */
object Qo100 {
    const val DOWNLINK_LOW_HZ = 10_489_500_000L
    const val DOWNLINK_HIGH_HZ = 10_490_000_000L
    const val UPLINK_LOW_HZ = 2_400_000_000L
    const val UPLINK_HIGH_HZ = 2_400_500_000L
    /** Downlink minus uplink for the narrowband transponder. */
    const val TRANSPONDER_SHIFT_HZ = 8_089_500_000L
    const val CW_BEACON_HZ = 10_489_500_000L
    const val PSK_BEACON_HZ = 10_489_750_000L
    const val UPPER_BEACON_HZ = 10_490_000_000L

    /**
     * Transmit range allowed by the app: the NB passband, minus a 5 kHz guard
     * next to the lower and upper beacons and around the PSK beacon.
     */
    const val TX_LOW_HZ = 2_400_005_000L
    const val TX_HIGH_HZ = 2_400_495_000L
    private const val PSK_GUARD_LOW_HZ = 2_400_245_000L
    private const val PSK_GUARD_HIGH_HZ = 2_400_255_000L

    fun uplinkFor(downlinkHz: Long): Long = downlinkHz - TRANSPONDER_SHIFT_HZ

    fun downlinkFor(uplinkHz: Long): Long = uplinkHz + TRANSPONDER_SHIFT_HZ

    fun isTransmitAllowed(uplinkHz: Long): Boolean =
        uplinkHz in TX_LOW_HZ..TX_HIGH_HZ && uplinkHz !in PSK_GUARD_LOW_HZ..PSK_GUARD_HIGH_HZ

    /**
     * Parses a frequency typed in MHz, with a comma or a dot as decimal separator
     * ("10489,750" or "10489.7505"). Returns null when the text is not a number.
     */
    fun parseMHz(text: String): Long? {
        val mhz = text.trim().replace(',', '.').toBigDecimalOrNull() ?: return null
        return mhz.movePointRight(6).setScale(0, java.math.RoundingMode.HALF_UP).longValueExact()
    }

    /** "10489,750.000" style display, kHz grouping like a transceiver dial. */
    fun formatMHz(hz: Long): String {
        val mhz = hz / 1_000_000
        val khz = (hz / 1_000) % 1_000
        val rest = hz % 1_000
        return String.format(Locale.FRANCE, "%d,%03d.%03d", mhz, khz, rest)
    }
}
