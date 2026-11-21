package ng.kithso.android.playback

import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.playback.BrowseTree.Companion.filledRecents
import ng.kithso.android.playback.BrowseTree.Companion.id
import ng.kithso.android.playback.BrowseTree.Companion.parseButton
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
    fun homeLeavesOutEmptyLists() {
        val art = ArtRef("", "")
        fun recent(kind: CollectionRef.Kind, uid: String) = Recent(CollectionRef(kind, uid), uid, null, art)
        val album = recent(CollectionRef.Kind.Album, "album")
        val filled = recent(CollectionRef.Kind.Playlist, "filled")
        val fullGroup = recent(CollectionRef.Kind.AlbumGroup, "full")
        val library = Library(
            recents = listOf(
                album,
                recent(CollectionRef.Kind.Playlist, "empty"),
                filled,
                recent(CollectionRef.Kind.AlbumGroup, "none"),
                fullGroup,
            ),
            playlists = listOf(Playlist("empty", "", 0, art), Playlist("filled", "", 2, art)),
            albums = emptyList(),
            artists = emptyList(),
            albumGroups = listOf(AlbumGroup("none", "", 0, art), AlbumGroup("full", "", 1, art)),
        )
        assertEquals(listOf(album, filled, fullGroup), library.filledRecents)
    }

    @Test
    fun aGroupAlbumIdWithoutAnAlbumIsNothing() {
        assertNull(parseRef("groupalbum:g"))
    }

    @Test
    fun buttonsNameTheirCollection() {
        val group = CollectionRef(CollectionRef.Kind.AlbumGroup, "g:1")
        assertEquals(BrowseTree.Button.Play to inGroup, parseButton("play:" + inGroup.id))
        assertEquals(BrowseTree.Button.Shuffle to plain, parseButton("shuffle:" + plain.id))
        assertEquals(BrowseTree.Button.ShuffleAlbums to group, parseButton("shufflealbums:" + group.id))
        // Shuffle albums is an album group's alone, and a playlist isn't a button.
        assertNull(parseButton("shufflealbums:" + plain.id))
        assertNull(parseButton("playlist:p"))
    }
}
