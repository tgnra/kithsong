package ng.kithso.android.ui

import androidx.compose.foundation.lazy.LazyListItemInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ReorderTest {
    private data class Item(override val index: Int, override val key: Any, override val offset: Int, override val size: Int) : LazyListItemInfo

    /** Lays [rows] (keys and heights) out one under another. */
    private fun layout(rows: List<Pair<String, Int>>): List<Item> {
        var y = 0
        return rows.mapIndexed { i, (key, size) -> Item(i, key, y, size).also { y += size } }
    }

    private fun move(rows: List<Pair<String, Int>>, key: String, centre: Int, isMovable: (Any) -> Boolean = { true }): ReorderMove? {
        val items = layout(rows)
        return reorderMove(items.first { it.key == key }, items, centre, isMovable)
    }

    /** [rows] once [key]'s been moved as far as it goes with its middle at [centre]; fails if it never settles. */
    private fun settle(rows: List<Pair<String, Int>>, key: String, centre: Int): List<String> {
        var now = rows
        repeat(rows.size) {
            val move = move(now, key, centre) ?: return now.map { it.first }
            val next = now.toMutableList().apply { add(move.target.index, removeAt(now.indexOfFirst { it.first == key })) }
            // It's laid out where the move said.
            assertEquals(move.offset, layout(next).first { it.key == key }.offset)
            now = next
        }
        throw AssertionError("$key at $centre doesn't settle")
    }

    private val even = listOf("a" to 100, "b" to 100, "c" to 100)
    private val uneven = listOf("a" to 60, "artists" to 100, "b" to 60, "c" to 60)

    @Test
    fun `items of one height are passed once the middle's over them`() {
        assertNull(move(even, "b", 199))
        assertEquals("c", move(even, "b", 200)?.target?.key)
        assertNull(move(even, "b", 100))
        assertEquals("a", move(even, "b", 99)?.target?.key)
    }

    @Test
    fun `a drag settles wherever it is, with items of any height`() {
        for (key in uneven.map { it.first }) for (centre in -50..350) settle(uneven, key, centre)
    }

    @Test
    fun `a drag down past a taller item and back leaves the order as it was`() {
        assertEquals(listOf("artists", "a", "b", "c"), settle(uneven, "a", 100))
        val down = listOf("artists" to 100, "a" to 60, "b" to 60, "c" to 60)
        assertEquals(listOf("artists", "a", "b", "c"), settle(down, "a", 60))
        assertEquals(listOf("a", "artists", "b", "c"), settle(down, "a", 30))
    }

    @Test
    fun `a drag up from below a taller item goes above it`() {
        assertEquals(listOf("a", "b", "artists", "c"), settle(uneven, "b", 100))
        assertEquals(listOf("b", "a", "artists", "c"), settle(uneven, "b", 30))
        assertEquals(listOf("c", "a", "artists", "b"), settle(uneven, "c", 30))
    }

    @Test
    fun `items that don't move aren't passed`() {
        assertNull(move(even, "b", 50) { it != "a" })
    }
}
