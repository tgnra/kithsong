package ng.kithso.android.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FastScrollTest {
    private val screen = 1000

    private fun heading(title: String, index: Int) = FastScrollEntry(title, index, heading = true)
    private fun letter(label: String, index: Int, section: String? = null) = FastScrollEntry(label, index, section)

    @Test
    fun offsetsAddUpTheRows() {
        val offsets = itemOffsets(3) { (it + 1) * 10 }
        assertEquals(listOf(0, 10, 30, 60), offsets.toList())
    }

    @Test
    fun aGridGetsAPlaceAScreen() {
        // A heading, then 40 grid rows of 250 px: four to a screen, so ten screens, whose few
        // letters alone would give three places.
        val offsets = itemOffsets(41) { if (it == 0) 60 else 250 }
        val places = groupEntries(
            listOf(heading("Albums", 0), letter("A", 1, "Albums"), letter("B", 20, "Albums"), letter("C", 30, "Albums")),
            offsets,
            screen,
        )
        assertTrue("${places.size} places", places.size in 10..12)
        // In order, and never much more than a screen apart.
        val tops = places.map { offsets[it.index] }
        assertEquals(tops.sorted(), tops)
        assertTrue(tops.zipWithNext { a, b -> b - a }.all { it <= screen + 250 })
    }

    @Test
    fun aTickIsUnderTheLetterBeforeIt() {
        val offsets = itemOffsets(40) { 250 }
        val places = groupEntries(listOf(letter("A", 0, "Albums"), letter("C", 30, "Albums")), offsets, screen)
        assertEquals("A", places[0].label)
        assertEquals(false, places[0].tick)
        assertEquals(4, places[1].index)
        assertEquals("A", places[1].label)
        assertEquals(true, places[1].tick)
        val c = places.first { it.label == "C" }
        assertEquals(false, c.tick)
        // After it, the ticks are C's, up to the last screen.
        assertEquals("C", places.last().label)
        assertEquals(true, places.last().tick)
        assertEquals(34, places.last().index)
    }

    @Test
    fun aHeadingWithNoLettersGetsTicksToo() {
        val offsets = itemOffsets(30) { 250 }
        val places = groupEntries(listOf(heading("Recents", 0), heading("Artists", 20)), offsets, screen)
        assertEquals(listOf(0, 4, 8, 12, 16, 20, 24), places.map { it.index })
        assertEquals(listOf(false, true, true, true, true, false, true), places.map { it.tick })
        assertTrue(places.all { it.heading })
    }

    @Test
    fun lettersWithinAScreenAreOnePlace() {
        val offsets = itemOffsets(100) { 100 }
        val places = groupEntries(
            listOf(letter("A", 0), letter("B", 3), letter("C", 6), letter("D", 12), letter("E", 15)),
            offsets,
            screen,
        )
        assertEquals("A", places[0].label)
        assertEquals("C", places[0].through)
        assertEquals("D", places[1].label)
        assertEquals("E", places[1].through)
    }

    @Test
    fun aShortListHasNoTicks() {
        val offsets = itemOffsets(8) { 100 }
        val places = groupEntries(listOf(letter("A", 0), letter("B", 4)), offsets, screen)
        assertEquals(1, places.size)
        assertEquals("B", places[0].through)
    }

    @Test
    fun aWiderStepMeansFewerPlaces() {
        // 400 screens on a strip with room for 80: a place every five.
        val offsets = itemOffsets(1600) { 250 }
        val places = groupEntries(listOf(letter("A", 0)), offsets, 400 * screen / 80)
        assertTrue("${places.size} places", places.size in 79..81)
    }

    @Test
    fun ticksAreDotsAndNothingCrowds() {
        val entries = listOf(
            heading("Albums", 0),
            letter("A", 1),
            letter("A", 5).copy(tick = true),
            letter("B", 9),
            letter("C", 13),
            letter("D", 17),
        )
        val spans = FloatArray(entries.size) { 0.1f }
        val starts = FloatArray(entries.size) { it * 0.1f }
        val marks = stripMarks(entries, starts, spans, minSlot = 0.15f, minTick = 0.08f)
        assertEquals(
            listOf(Mark.Label, Mark.Dot, Mark.Dot, Mark.Label, Mark.Dot, Mark.Label),
            marks.toList(),
        )
        val tight = stripMarks(entries, starts, spans, minSlot = 0.3f, minTick = 0.15f)
        assertEquals(
            listOf(Mark.Label, Mark.None, Mark.Dot, Mark.None, Mark.Label, Mark.None),
            tight.toList(),
        )
    }
}
