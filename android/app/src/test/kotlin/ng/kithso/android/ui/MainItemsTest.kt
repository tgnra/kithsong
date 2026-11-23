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
                MainItem.RecentsRow(recents(30).take(PHONE_RECENTS)), // the sidebar takes more
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

    // A to Z, every fourth from B only credited on others' tracks: 19 of their own and 7 credited.
    private fun someCredited() = List(26) { Artist("x$it", "${'A' + it} artist", 1, art, creditOnly = it % 4 == 1) }

    private fun artistsOf(items: List<MainItem>) = items.filterIsInstance<MainItem.ArtistItem>().map { it.artist }

    @Test
    fun creditedArtistsFoldAwayUnderARowAtTheEnd() {
        val all = someCredited()
        val (credited, own) = all.partition { it.creditOnly }
        val library = Library(emptyList(), emptyList(), emptyList(), all, emptyList())
        val closed = mainItems(library, 2, false, false, foldCredited = true)
        assertEquals(own, artistsOf(closed))
        assertEquals(MainItem.CreditedArtistsRow(7, open = false), closed.last())
        val open = mainItems(library, 2, false, false, foldCredited = true, creditedOpen = true)
        assertEquals(own + credited, artistsOf(open))
        assertEquals(MainItem.CreditedArtistsRow(7, open = true), open[open.size - 8])
        // Not folded, they're all in order, with no row.
        val unfolded = mainItems(library, 2, false, false, foldCredited = false, creditedOpen = true)
        assertEquals(all, artistsOf(unfolded))
        assertTrue(unfolded.none { it is MainItem.CreditedArtistsRow })
    }

    @Test
    fun theStripHasTheFoldedArtistsLettersOnlyWhileOpen() {
        val library = Library(emptyList(), emptyList(), emptyList(), someCredited(), emptyList())
        val closed = mainItems(library, 2, false, false, foldCredited = true)
        val closedLetters = entries(closed).filter { it.section == "Artists" }
        assertEquals(someCredited().filter { !it.creditOnly }.map { it.name.take(1) }, closedLetters.map { it.label })

        // Open, the folded artists' letters follow, each pointing at its artist under the row.
        val open = mainItems(library, 2, false, false, foldCredited = true, creditedOpen = true)
        val row = open.indexOfFirst { it is MainItem.CreditedArtistsRow }
        val openLetters = entries(open).filter { it.section == "Artists" }
        assertEquals(closedLetters, openLetters.take(19))
        val after = openLetters.drop(19)
        assertEquals(listOf("B", "F", "J", "N", "R", "V", "Z"), after.map { it.label })
        for (entry in after) {
            assertTrue(entry.index > row)
            assertEquals(entry.label, (open[entry.index] as MainItem.ArtistItem).artist.name.take(1))
        }
    }

    @Test
    fun openingTheFoldCanBringTheArtistsUpToHavingLetters() {
        val artists = List(12) { Artist("x$it", "${'A' + it} artist", 1, art, creditOnly = it >= 5) }
        val library = Library(emptyList(), emptyList(), emptyList(), artists, emptyList())
        val closed = mainItems(library, 2, false, false, foldCredited = true)
        assertTrue(entries(closed).none { it.section == "Artists" })
        val open = mainItems(library, 2, false, false, foldCredited = true, creditedOpen = true)
        assertEquals(12, entries(open).count { it.section == "Artists" })
    }

    @Test
    fun `Recents stays first whatever order was saved`() {
        assertEquals(
            listOf(Section.Recents, Section.Artists, Section.Albums, Section.Playlists, Section.AlbumGroups),
            orderedSections(listOf("Artists", "Recents", "Albums")),
        )
        assertEquals(Section.entries, orderedSections(emptyList()))
    }
}
