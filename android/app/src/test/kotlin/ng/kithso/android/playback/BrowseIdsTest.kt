package ng.kithso.android.playback

import ng.kithso.android.data.CollectionRef
import ng.kithso.android.playback.BrowseTree.Companion.id
import ng.kithso.android.playback.BrowseTree.Companion.parseRef
import ng.kithso.android.playback.BrowseTree.Companion.parseTrack
import ng.kithso.android.playback.BrowseTree.Companion.trackId
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class BrowseIdsTest {
    private val plain = CollectionRef(CollectionRef.Kind.Album, "a:1")
    private val inGroup = CollectionRef(CollectionRef.Kind.Album, "a:1", "g:1")

    @Test
    fun anAlbumKeepsTheGroupItWasListedUnder() {
        assertEquals("album:a:1", plain.id)
        assertEquals(plain, parseRef(plain.id))
        assertEquals(inGroup, parseRef(inGroup.id))
        assertEquals(inGroup to 3, parseTrack(trackId(inGroup, 3)))
    }

    @Test
    fun aGroupAlbumIdWithoutAnAlbumIsNothing() {
        assertNull(parseRef("groupalbum:g"))
    }
}
