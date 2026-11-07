package fr.yvespetit.androidnb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class Qo100Test {
    @Test fun parsesCommaAndDot() {
        assertEquals(10_489_750_000L, Qo100.parseMHz("10489,750"))
        assertEquals(10_489_750_500L, Qo100.parseMHz(" 10489.7505 "))
        assertNull(Qo100.parseMHz("abc"))
    }

    @Test fun uplinkMapsTheTransponder() {
        assertEquals(2_400_250_000L, Qo100.uplinkFor(10_489_750_000L))
        assertEquals(10_489_550_000L, Qo100.downlinkFor(2_400_050_000L))
    }

    @Test fun transmitStaysInsideTheNarrowbandPassband() {
        assertTrue(Qo100.isTransmitAllowed(2_400_400_000L))
        assertFalse(Qo100.isTransmitAllowed(2_400_000_000L)) // lower beacon
        assertFalse(Qo100.isTransmitAllowed(2_400_250_000L)) // PSK beacon
        assertFalse(Qo100.isTransmitAllowed(2_400_550_000L)) // above the transponder
    }

    @Test fun formatsLikeADial() {
        assertEquals("10489,750.000", Qo100.formatMHz(10_489_750_000L))
        assertEquals("2400,250.500", Qo100.formatMHz(2_400_250_500L))
    }
}
