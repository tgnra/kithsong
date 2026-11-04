package ng.kithso.android.ui

import ng.kithso.android.data.Album
import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MainItemsTest {
    private val art = ArtRef("art", "art")
    private val titles = Section.entries.associateWith { it.name }

    private fun recents(n: Int) = List(n) { Recent(CollectionRef(CollectionRef.Kind.Album, "r$it"), "Recent $it", "Artist", art) }
    private fun groups(n: Int) = List(n) { AlbumGroup("g$it", "${'A' + it % 26} group", 3, art) }
    private fun playlists(n: Int) = List(n) { Playlist("p$it", "${'A' + it % 26} list", 3, art) }
    private fun albums(n: Int) = List(n) { Album("a$it", "${'A' + it % 26} album", "Artist", 2000, art) }
    private fun artists(n: Int) = List(n) { Artist("x$it", "${'A' + it % 26} artist", 1, art) }

    private fun library(recents: Int = 0, playlists: Int = 0, albums: Int = 0, artists: Int = 0, groups: Int = 0) =
        Library(recents(recents), playlists(playlists), albums(albums), artists(artists), groups(groups))

    private fun entries(items: List<MainItem>) = fastScrollEntries(items, titles, "#")

    @Test
    fun recentsAndAlbumGroupsAreARowEach() {
        val items = mainItems(library(recents = 30, albums = 1, groups = 30), columns = 2, banner = false, canMake = false)
        assertEquals(
            listOf(
                MainItem.Header,
                MainItem.Heading(Section.Recents),
                MainItem.RecentsRow(recents(30)),
                MainItem.Heading(Section.AlbumGroups),
                MainItem.AlbumGroupsRow(groups(30)),
                MainItem.Heading(Section.Albums),
            ),
            items.take(6),
        )
    }

    @Test
    fun aRowOnlyCountsItsFirstTilesArt() {
        val row = MainItem.AlbumGroupsRow(groups(100))
        assertTrue(row.art.size < 10)
    }

    @Test
    fun noGroupsLeavesTheHeadingForItsPlusButNoRow() {
        val items = mainItems(library(albums = 1), columns = 2, banner = false, canMake = true)
        assertTrue(MainItem.Heading(Section.AlbumGroups) in items)
        assertTrue(items.none { it is MainItem.AlbumGroupsRow })
    }

    @Test
    fun anEmptyLibraryIsTheEmptyState() {
        val items = mainItems(Library.Empty, columns = 2, banner = false, canMake = true)
        assertEquals(listOf(MainItem.Header, MainItem.Empty), items)
    }

    @Test
    fun theStripPointsAtHeadingsPastTheRows() {
        val items = mainItems(library(recents = 40, playlists = 2, albums = 3, artists = 2, groups = 40), 2, false, false)
        val entries = entries(items)
        assertEquals(
            listOf("Recents", "Playlists", "AlbumGroups", "Albums", "Artists"),
            entries.map { it.label },
        )
        assertTrue(entries.all { it.heading })
        for (entry in entries) {
            val heading = items[entry.index] as MainItem.Heading
            assertEquals(entry.label, heading.section.name)
        }
    }

    @Test
    fun aLongRowHasNoLettersButTheSectionsAfterItKeepThem() {
        val items = mainItems(library(recents = 4, albums = 26, artists = 26, groups = 26), columns = 2, banner = true, canMake = false)
        val entries = entries(items)
        assertTrue(entries.none { it.section == "AlbumGroups" })
        assertTrue(entries.none { it.section == "Recents" })
        val albumLetters = entries.filter { it.section == "Albums" }
        assertEquals(26, albumLetters.size)
        // Each letter points at the grid row with that album in it.
        for ((i, entry) in albumLetters.withIndex()) {
            val row = items[entry.index] as MainItem.AlbumRow
            assertTrue(row.albums.any { it.title.first() == 'A' + i })
        }
        for (entry in entries.filter { it.section == "Artists" }) {
            val artist = items[entry.index] as MainItem.ArtistItem
            assertEquals(entry.label.first(), artist.artist.sortName.first())
        }
    }
}
