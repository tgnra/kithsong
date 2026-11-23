package ng.kithso.android.ui

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.unit.dp
import ng.kithso.android.data.Album
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Library
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

class AlbumPinchTest {
    @Test
    fun `a pinch switches only past the threshold, and only to the other size`() {
        assertNull(pinchedSmall(1.1f, small = true))
        assertNull(pinchedSmall(0.9f, small = false))
        assertEquals(false, pinchedSmall(1.3f, small = true))
        assertEquals(true, pinchedSmall(0.7f, small = false))
        // Already that size.
        assertNull(pinchedSmall(1.5f, small = false))
        assertNull(pinchedSmall(0.5f, small = true))
    }

    @Test
    fun `a phone in portrait has two columns, or three with small tiles`() {
        assertEquals(2, phoneAlbumColumns(411.dp, small = false))
        assertEquals(3, phoneAlbumColumns(411.dp, small = true))
    }

    @Test
    fun `the pinched album's row is where the list goes`() {
        // Album 7 of a grid from item 4: row 3 at two across, row 2 at three.
        assertEquals(4 + 3, keptAlbumPlace(7, 4, 2, y = 0, rowTop = 0, rowHeight = 100, newRowHeight = 100).index)
        assertEquals(4 + 2, keptAlbumPlace(7, 4, 3, y = 0, rowTop = 0, rowHeight = 100, newRowHeight = 100).index)
    }

    @Test
    fun `the point pinched stays as far down its row`() {
        // Halfway down a 300px row at 500px: halfway down the 200px row it becomes, still at 500px.
        val place = keptAlbumPlace(0, 4, 3, y = 500, rowTop = 350, rowHeight = 300, newRowHeight = 200)
        assertEquals(100 - 500, place.offset)
        // A row's top, kept at its place on screen.
        assertEquals(-120, keptAlbumPlace(0, 4, 3, y = 120, rowTop = 120, rowHeight = 300, newRowHeight = 200).offset)
    }

    @Test
    fun `the fast scroll strip tells rows of different widths apart, and its letters follow the rows`() {
        val art = ArtRef("art", "art")
        val library = Library(emptyList(), emptyList(), List(26) { Album("a$it", "${'A' + it} album", "Artist", 2000, art) }, emptyList(), emptyList())
        val titles = Section.entries.associateWith { it.name }
        val two = mainItems(library, columns = 2, banner = false, canMake = false)
        val three = mainItems(library, columns = 3, banner = false, canMake = false)
        assertNotEquals(two.last().contentType, three.last().contentType)
        for (items in listOf(two, three)) {
            for (entry in fastScrollEntries(items, titles, "#").filter { !it.heading }) {
                val row = items[entry.index] as MainItem.AlbumRow
                assertEquals(true, row.albums.any { it.title.first() == entry.label.first() })
            }
        }
    }

    @Test
    fun `a reflowing tile starts where it was, at its old size, and ends where it is`() {
        // A 200px tile at (16, 500) becomes a 130px one at (160, 300).
        val from = Rect(16f, 500f, 216f, 760f)
        val start = tileShift(from, Offset(160f, 300f), 130f, 0f)
        assertEquals(TileShift(-144f, 200f, 200f / 130f), start)
        val end = tileShift(from, Offset(160f, 300f), 130f, 1f)
        assertEquals(0f, end.dx, 0.001f)
        assertEquals(0f, end.dy, 0.001f)
        assertEquals(1f, end.scale, 0.001f)
        val half = tileShift(from, Offset(160f, 300f), 130f, 0.5f)
        assertEquals(-72f, half.dx, 0.001f)
        assertEquals(100f, half.dy, 0.001f)
        assertEquals((200f / 130f + 1) / 2, half.scale, 0.001f)
    }
}
