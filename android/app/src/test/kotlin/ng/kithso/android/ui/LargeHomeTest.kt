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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LargeHomeTest {
    private val art = ArtRef("art", "art")
    private fun albums(n: Int) = List(n) { Album("a$it", "Album $it", "Artist", 2000, art) }
    private val artist = Artist("x", "Artist", 1, art)
    private val playlist = Playlist("p", "List", 3, art)
    private val group = AlbumGroup("g", "Group", 3, art)
    private val recent = Recent(CollectionRef(CollectionRef.Kind.Album, "a0"), "Recent", "Artist", art)
    private fun stations(n: Int) = List(n) { Station("s$it", "Station $it", null, "Seed", art) }

    private val everything = Library(listOf(recent), listOf(playlist), albums(5), listOf(artist), listOf(group), stations(5))

    @Test
    fun theCategoriesAreThoseWithSomethingInThemInTheMainListsOrder() {
        assertEquals(
            listOf(Section.Radio, Section.Recents, Section.Playlists, Section.AlbumGroups, Section.Albums, Section.Artists),
            categories(everything),
        )
        assertEquals(listOf(Section.Albums), categories(Library.Empty.copy(albums = albums(1))))
        assertEquals(emptyList<Section>(), categories(Library.Empty))
    }

    @Test
    fun albumsAreShownUntilSomethingElseIsChosen() {
        assertEquals(Section.Albums, shownCategory(null, null))
        assertEquals(Section.Albums, shownCategory(everything, null))
        assertEquals(Section.Artists, shownCategory(everything, Section.Artists))
    }

    @Test
    fun whatsChosenStaysWhileTheLibraryIsBeingRead() {
        assertEquals(Section.Radio, shownCategory(null, Section.Radio))
    }

    @Test
    fun aCategoryThatEmptiesGivesWayToAlbumsOrTheFirstThereIs() {
        val noPlaylists = everything.copy(playlists = emptyList())
        assertEquals(Section.Albums, shownCategory(noPlaylists, Section.Playlists))
        assertEquals(Section.Playlists, shownCategory(Library.Empty.copy(playlists = listOf(playlist)), Section.Albums))
        assertEquals(Section.Albums, shownCategory(Library.Empty, Section.Radio))
    }

    @Test
    fun aCategoryIsItsHeadingAndItsItemsAlone() {
        assertEquals(
            listOf(
                MainItem.Top,
                MainItem.Heading(Section.Albums),
                MainItem.AlbumRow(albums(5).take(2)),
                MainItem.AlbumRow(albums(5).subList(2, 4)),
                MainItem.AlbumRow(albums(5).drop(4)),
            ),
            categoryItems(everything, Section.Albums, columns = 2, banner = false),
        )
        assertEquals(
            listOf(MainItem.Top, MainItem.Heading(Section.Artists), MainItem.ArtistItem(artist)),
            categoryItems(everything, Section.Artists, columns = 2, banner = false),
        )
    }

    @Test
    fun tilesThatAreARowOnThePhoneAreAGridInACategory() {
        val rows = categoryItems(everything, Section.Radio, columns = 3, banner = false).filterIsInstance<MainItem.RadioRow>()
        assertEquals(listOf(3, 2), rows.map { it.stations.size })
        assertEquals(listOf(0, 1), rows.map { it.gridRow })
    }

    @Test
    fun theBannerGoesAboveTheCategory() {
        val items = categoryItems(everything, Section.Albums, columns = 2, banner = true)
        assertEquals(listOf(MainItem.Top, MainItem.Banner, MainItem.Heading(Section.Albums)), items.take(3))
    }

    @Test
    fun anEmptyLibraryWelcomesInTheMiddlePane() {
        assertEquals(listOf(MainItem.Top, MainItem.Empty), categoryItems(Library.Empty, Section.Albums, columns = 2, banner = false))
    }

    @Test
    fun aCategoryHasNothingYetWhileTheLibraryIsBeingRead() {
        assertEquals(listOf<MainItem>(MainItem.Top), categoryItems(null, Section.Albums, columns = 2, banner = false))
    }

    @Test
    fun aLongCategoryHasItsLettersToScrollBy() {
        val items = categoryItems(everything.copy(albums = albums(30)), Section.Albums, columns = 2, banner = false)
        assertTrue(fastScrollEntries(items, Section.entries.associateWith { it.name }, "#").size > 1)
        assertFalse(items.any { it is MainItem.ArtistItem })
    }
}
