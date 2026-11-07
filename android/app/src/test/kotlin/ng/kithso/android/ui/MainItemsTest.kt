package ng.kithso.android.ui

import ng.kithso.android.data.Album
import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.data.Station
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
    fun radioIsARowDirectlyAboveRecents() {
        val stations = List(3) { Station("s$it", "Station $it", "Station", "Seed", art) }
        val library = library(recents = 2, albums = 1).copy(stations = stations)
        val items = mainItems(library, columns = 2, banner = false, canMake = false)
        assertEquals(
            listOf(
                MainItem.Header,
                MainItem.Heading(Section.Radio),
                MainItem.RadioRow(stations),
                MainItem.Heading(Section.Recents),
                MainItem.RecentsRow(recents(2)),
            ),
            items.take(5),
        )
        assertEquals(listOf("Radio", "Recents", "Albums"), entries(items).map { it.label })
    }

    @Test
    fun aLibraryWithNoStationsHasNoRadioRow() {
        val items = mainItems(library(recents = 2, albums = 1), columns = 2, banner = false, canMake = false)
        assertTrue(MainItem.Heading(Section.Radio) !in items)
        assertTrue(items.none { it is MainItem.RadioRow })
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

    @Test
    fun theLeftColumnHasNoSidewaysRowsAndNoAlbums() {
        val stations = List(3) { Station("s$it", "Station $it", "Station", "Seed", art) }
        val library = library(recents = 3, playlists = 1, albums = 5, artists = 2, groups = 3).copy(stations = stations)
        val items = leftColumnItems(library, columns = 2, banner = true, canMake = false)
        assertEquals(
            listOf(
                MainItem.Header,
                MainItem.Banner,
                MainItem.Heading(Section.Radio),
                MainItem.RadioRow(stations.take(2), 0),
                MainItem.RadioRow(stations.drop(2), 1),
                MainItem.Heading(Section.Recents),
                MainItem.RecentsRow(recents(3).take(2), 0),
                MainItem.RecentsRow(recents(3).drop(2), 1),
                MainItem.Heading(Section.Playlists),
                MainItem.PlaylistItem(playlists(1).first()),
                MainItem.Heading(Section.AlbumGroups),
                MainItem.AlbumGroupsRow(groups(3).take(2), 0),
                MainItem.AlbumGroupsRow(groups(3).drop(2), 1),
            ),
            items,
        )
        assertEquals(items.size, items.map { it.key }.toSet().size)
    }

    @Test
    fun aGridsRowsAreEstimatedAsAlbumRowsAre() {
        val items = leftColumnItems(library(recents = 2), columns = 2, banner = false, canMake = false)
        assertEquals(MainItem.AlbumRow(albums(1)).contentType, items.last().contentType)
    }

    @Test
    fun theMiddleColumnIsAlbumsThenArtists() {
        val items = middleColumnItems(library(recents = 3, albums = 3, artists = 1), columns = 2)
        assertEquals(
            listOf(
                MainItem.Top,
                MainItem.Heading(Section.Albums),
                MainItem.AlbumRow(albums(3).take(2)),
                MainItem.AlbumRow(albums(3).drop(2)),
                MainItem.Heading(Section.Artists),
                MainItem.ArtistItem(artists(1).first()),
            ),
            items,
        )
    }

    @Test
    fun anEmptyLibraryIsInTheLeftColumnAndTheMiddleOneHasNothing() {
        val empty = library()
        assertTrue(MainItem.Empty in leftColumnItems(empty, columns = 2, banner = false, canMake = true))
        assertEquals(listOf<MainItem>(MainItem.Top), middleColumnItems(empty, columns = 2))
    }
}
