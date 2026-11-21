package ng.kithso.android.ui

import androidx.compose.ui.unit.dp
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import ng.kithso.android.data.ArtRef
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class TileRowTest {
    // What's visible of a row of tiles across [width]: the gutter, the whole tiles with the gaps
    // after them, and what's left of the next.
    private fun slice(width: Float, columns: Int): Float {
        val tile = tileWidth(width.dp, columns).value
        return width - BrowseGutter.value - columns * (tile + TileSpacing.value)
    }

    @Test
    fun aPhoneShowsThreeTilesAndASlice() {
        for (width in listOf(320f, 360f, 411f, 480f)) {
            assertEquals(3, tileColumns(width.dp))
            val tile = tileWidth(width.dp).value
            assertEquals(tile * 0.25f, slice(width, 3), 0.01f)
        }
    }

    @Test
    fun widerScreensShowMoreTilesRatherThanHugeOnes() {
        assertEquals(4, tileColumns(800.dp))
        assertEquals(7, tileColumns(1280.dp))
        for (width in 300..2000 step 20) {
            val tile = tileWidth(width.dp).value
            assertTrue("$tile at $width", tile <= 176f)
            // Always a slice, whatever the width.
            val columns = tileColumns(width.dp)
            assertEquals(tile * 0.25f, slice(width.toFloat(), columns), 0.01f)
        }
    }

    @Test
    fun aTileWithNoArtGetsAPlaceholderFromItsTitle() {
        val art = Tile("mix-1", "朝の音楽") {}.shownArt()
        assertEquals("mix-1", art.uid)
        assertEquals("朝の音楽", art.label)
        assertEquals(null, art.file)
        val own = ArtRef("a", "b", "c.jpg")
        assertEquals(own, Tile("mix-1", "朝の音楽", art = own) {}.shownArt())
    }

    @Test
    fun aRowOffScreenLongEnoughResets() = runTest {
        val onScreen = MutableStateFlow(true)
        var resets = 0
        val job = launch { resetAfterOffScreen(onScreen) { resets++ } }
        runCurrent()
        onScreen.value = false
        advanceTimeBy(TILE_ROW_RESET_MS - 1)
        runCurrent()
        assertEquals(0, resets)
        advanceTimeBy(1)
        runCurrent()
        assertEquals(1, resets)
        // Once for each time it leaves.
        advanceTimeBy(TILE_ROW_RESET_MS * 2)
        runCurrent()
        assertEquals(1, resets)
        job.cancel()
    }

    @Test
    fun aQuickReturnKeepsThePosition() = runTest {
        val onScreen = MutableStateFlow(true)
        var resets = 0
        val job = launch { resetAfterOffScreen(onScreen) { resets++ } }
        runCurrent()
        onScreen.value = false
        advanceTimeBy(TILE_ROW_RESET_MS - 1)
        runCurrent()
        onScreen.value = true
        runCurrent()
        advanceTimeBy(TILE_ROW_RESET_MS * 2)
        runCurrent()
        assertEquals(0, resets)
        // And leaving again counts from the start.
        onScreen.value = false
        advanceTimeBy(TILE_ROW_RESET_MS - 1)
        runCurrent()
        assertEquals(0, resets)
        job.cancel()
    }

    @Test
    fun aRowNotYetOnScreenKeepsItsPlace() = runTest {
        // A turned phone brings the list back with the row below the fold.
        val onScreen = MutableStateFlow(false)
        var resets = 0
        val job = launch { resetAfterOffScreen(onScreen) { resets++ } }
        advanceTimeBy(TILE_ROW_RESET_MS * 3)
        runCurrent()
        assertEquals(0, resets)
        job.cancel()
    }
}
