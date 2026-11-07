package ng.kithso.android.ui

import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FoldGridTest {
    @Test
    fun `with no fold the columns are the phone's`() {
        val columns = gridColumns(600.dp, null)
        assertEquals(albumColumns(600.dp), columns.count)
        assertEquals(BrowseGutter.value, columns.starts.first().value, EPSILON)
        assertEquals(600f - BrowseGutter.value, (columns.starts.last() + columns.tileWidth).value, EPSILON)
    }

    @Test
    fun `a fold outside the tiles changes nothing`() {
        assertEquals(gridColumns(600.dp, null), gridColumns(600.dp, FoldSpan(4.dp, 8.dp)))
        assertEquals(gridColumns(600.dp, null), gridColumns(600.dp, FoldSpan(700.dp, 700.dp)))
    }

    @Test
    fun `no tile crosses a fold with no width`() {
        for (at in listOf(180, 250, 300, 333, 420)) {
            val fold = FoldSpan(at.dp, at.dp)
            assertClear(gridColumns(601.dp, fold), fold)
        }
    }

    @Test
    fun `no tile crosses a hinge`() {
        val fold = FoldSpan(300.dp, 334.dp)
        assertClear(gridColumns(700.dp, fold), fold)
    }

    @Test
    fun `both sides of the fold have tiles of one width`() {
        // The Pixel Fold held sideways, the sidebar before the grid: the fold 180dp in.
        val fold = FoldSpan(180.dp, 180.dp)
        val columns = gridColumns(601.dp, fold)
        assertEquals(3, columns.count)
        assertEquals(1, columns.starts.count { it < fold.start })
        assertTrue(columns.tileWidth >= MinTileWidth)
    }

    @Test
    fun `a narrow side gets narrower tiles rather than none`() {
        // Room for a 130dp tile before the fold, but not a full-width one.
        val fold = FoldSpan(BrowseGutter + 130.dp + FoldClearance, BrowseGutter + 130.dp + FoldClearance)
        val columns = gridColumns(600.dp, fold)
        assertTrue(columns.starts.any { it < fold.start })
        assertTrue(columns.tileWidth >= FoldMinTileWidth)
        assertClear(columns, fold)
    }

    private fun assertClear(columns: GridColumns, fold: FoldSpan) {
        for (start in columns.starts) {
            val end: Dp = start + columns.tileWidth
            assertTrue("tile $start..$end crosses $fold", end <= fold.start || start >= fold.end)
        }
        // In order, with no overlaps.
        columns.starts.zipWithNext { a, b -> assertTrue(b - a >= columns.tileWidth) }
    }

    private companion object {
        const val EPSILON = 0.01f
    }
}
