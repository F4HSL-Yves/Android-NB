package fr.yvespetit.androidnb.nb

import org.junit.Assert.assertTrue
import org.junit.Test

class BoardFilesTest {
    @Test
    fun tagsCompareAsVersions() {
        assertTrue(BoardFiles.compareTags("v1.17", "v1.9") > 0)
        assertTrue(BoardFiles.compareTags("v1.16", "v1.17") < 0)
        assertTrue(BoardFiles.compareTags("v1.17", "v1.17") == 0)
        assertTrue(BoardFiles.compareTags("v2.0", "v1.99") > 0)
        assertTrue(BoardFiles.compareTags("v1.17", "v0.1") > 0)  // a dev build's own files
    }
}
