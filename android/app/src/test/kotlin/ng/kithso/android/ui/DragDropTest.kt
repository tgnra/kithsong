package ng.kithso.android.ui

import ng.kithso.android.data.CollectionRef
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DragDropTest {
    private val track = DragItem.Track("t1", "Teardrop", "Massive Attack")
    private val album = DragItem.Album("a1", "Mezzanine", "Massive Attack")
    private val playlists = setOf("road-trip")
    private val groups = setOf("ambient")

    private fun playlist(uid: String) = CollectionRef(CollectionRef.Kind.Playlist, uid)
    private fun group(uid: String) = CollectionRef(CollectionRef.Kind.AlbumGroup, uid)

    @Test
    fun `a track drops on a manual playlist`() {
        assertTrue(canDrop(track, playlist("road-trip"), playlists, groups))
    }

    @Test
    fun `an album drops on a manual album group`() {
        assertTrue(canDrop(album, group("ambient"), playlists, groups))
    }

    @Test
    fun `smart lists take nothing`() {
        assertFalse(canDrop(track, playlist("most-played"), playlists, groups))
        assertFalse(canDrop(album, group("recently-added"), playlists, groups))
    }

    @Test
    fun `a track doesn't drop on an album group, nor an album on a playlist`() {
        // Even where a group and a playlist share a uid, as made-up ones might.
        assertFalse(canDrop(track, group("ambient"), playlists + "ambient", groups))
        assertFalse(canDrop(album, playlist("road-trip"), playlists, groups + "road-trip"))
    }

    @Test
    fun `nothing drops on an album, an artist or a station`() {
        for (kind in listOf(CollectionRef.Kind.Album, CollectionRef.Kind.Artist, CollectionRef.Kind.Station)) {
            val ref = CollectionRef(kind, "road-trip")
            assertFalse(canDrop(track, ref, playlists, setOf("road-trip")))
            assertFalse(canDrop(album, ref, playlists, setOf("road-trip")))
        }
    }

    @Test
    fun `other apps get the title and artist`() {
        assertEquals("Teardrop — Massive Attack", track.text())
        assertEquals("Mezzanine — Massive Attack", album.text())
        assertEquals("Untitled", DragItem.Track("t2", "Untitled", "").text())
    }
}
