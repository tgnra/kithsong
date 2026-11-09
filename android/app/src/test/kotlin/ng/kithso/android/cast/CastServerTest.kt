package ng.kithso.android.cast

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CastServerTest {
    @Test
    fun ranges() {
        assertEquals(0L to 999L, CastServer.parseRange("bytes=0-", 1000))
        assertEquals(100L to 199L, CastServer.parseRange("bytes=100-199", 1000))
        // Past the end is cut to the end; the last n bytes.
        assertEquals(900L to 999L, CastServer.parseRange("bytes=900-5000", 1000))
        assertEquals(800L to 999L, CastServer.parseRange("bytes=-200", 1000))
        assertEquals(0L to 999L, CastServer.parseRange("bytes=-5000", 1000))
        // Only the first of several.
        assertEquals(0L to 9L, CastServer.parseRange("bytes=0-9, 20-29", 1000))
    }

    @Test
    fun rangesThatArentAny() {
        assertNull(CastServer.parseRange("bytes=1000-", 1000))
        assertNull(CastServer.parseRange("bytes=500-100", 1000))
        assertNull(CastServer.parseRange("items=0-10", 1000))
        assertNull(CastServer.parseRange("bytes=abc", 1000))
        assertNull(CastServer.parseRange("bytes=0-", 0))
    }

    @Test
    fun typesByExtension() {
        assertEquals("audio/flac", CastServer.mimeType("01 Song.flac"))
        assertEquals("audio/mpeg", CastServer.mimeType("song.MP3"))
        assertEquals("audio/ogg", CastServer.mimeType("song.opus"))
    }
}
