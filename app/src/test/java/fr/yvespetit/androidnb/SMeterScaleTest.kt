package fr.yvespetit.androidnb

import org.junit.Assert.assertEquals
import org.junit.Test

class SMeterScaleTest {
    @Test fun labelsWithS9AtMinus73() {
        val s9 = -73.0
        assertEquals("S9", SMeterScale.label(-73.0, s9))
        assertEquals("S8", SMeterScale.label(-74.0, s9))
        assertEquals("S1", SMeterScale.label(-121.0, s9))
        assertEquals("S0", SMeterScale.label(-140.0, s9))
        assertEquals("S9", SMeterScale.label(-64.0, s9))
        assertEquals("S9+10", SMeterScale.label(-63.0, s9))
        assertEquals("S9+20", SMeterScale.label(-45.0, s9))
        assertEquals("S7", SMeterScale.label(-102.0, -90.0))
    }

    @Test fun barPosition() {
        val s9 = -73.0
        assertEquals(SMeterScale.S9_FRACTION, SMeterScale.fraction(-73.0, s9), 1e-9)
        assertEquals(0.0, SMeterScale.fraction(-200.0, s9), 1e-9)
        assertEquals(1.0, SMeterScale.fraction(-13.0, s9), 1e-9)
        assertEquals(1.0, SMeterScale.fraction(0.0, s9), 1e-9)
        assertEquals(SMeterScale.S9_FRACTION + 0.2, SMeterScale.fraction(-43.0, s9), 1e-9)
    }

    @Test fun defaultPutsQuietBandAtS2() {
        assertEquals("S2", SMeterScale.label(-58.7))
    }

    @Test fun noiseAtS2() {
        val cal = SMeterScale.s9ForNoiseAtS2(-70.2)
        assertEquals(-29.0, cal, 1e-9)
        assertEquals("S2", SMeterScale.label(-70.2, cal))
    }
}
