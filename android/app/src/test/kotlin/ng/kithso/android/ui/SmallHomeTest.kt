package ng.kithso.android.ui

import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.Playlist
import ng.kithso.android.ui.HomePage.Category
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class SmallHomeTest {
    private fun playlist(tracks: Int) = Playlist("uid", "Name", tracks, ArtRef("uid", "Name"))

    @Test
    fun anEmptyLibraryWelcomesThenHasTheUtilities() {
        assertEquals(listOf(HomePage.Welcome, HomePage.Utilities), homePages(Library.Empty))
    }

    @Test
    fun anEmptyPlaylistHasItsPageNextToTheWelcome() {
        val library = Library.Empty.copy(playlists = listOf(playlist(0)))
        assertEquals(listOf(HomePage.Welcome, Category(Section.Playlists), HomePage.Utilities), homePages(library))
    }

    @Test
    fun onlySectionsWithSomethingHavePagesAndTheUtilitiesComeLast() {
        val library = Library.Empty.copy(playlists = listOf(playlist(3)))
        assertEquals(listOf(Category(Section.Playlists), HomePage.Utilities), homePages(library))
    }

    @Test
    fun smallWindowsFitTwoOrThreeTilesAcross() {
        // In scaled dp: the Jelly, the Sidephone and the Flip's cover at LayoutScale.Dense's size.
        val size = LayoutScale.Dense.size
        assertEquals(2, smallColumns(240.dp / size))
        assertEquals(2, smallColumns(274.dp / size))
        assertEquals(3, smallColumns(316.dp / size))
    }

    @Test
    fun aRegularScaleLeavesTheDensityAlone() {
        val density = Density(2f, 1.3f)
        assertSame(density, density.scaledBy(LayoutScale.Regular))
    }

    @Test
    fun aDenseScaleShrinksDpAndSpEachByItsOwnAmount() {
        val scale = LayoutScale(font = 0.9f, size = 0.8f)
        val dense = Density(2f, 1.5f).scaledBy(scale)
        assertEquals(2f * 0.8f * 10f, with(dense) { 10.dp.toPx() }, 0.001f)
        // A sp is the system's size for it, the font scale and all, made that much smaller.
        assertEquals(2f * 1.5f * 0.9f * 10f, with(dense) { 10.sp.toPx() }, 0.001f)
        assertEquals(10f, with(dense) { 10.sp.toDp().toSp() }.value, 0.001f)
    }

    @Test
    fun denseKeepsTouchTargetsAtFortyDp() {
        assertTrue(48f * LayoutScale.Dense.size >= 40f)
    }
}
