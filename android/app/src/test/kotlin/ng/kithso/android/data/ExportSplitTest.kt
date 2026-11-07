package ng.kithso.android.data

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The export's zips (splitReleases): each at most the importer's limit, in the library's order,
 * with no release split across two; one too big for any zip goes alone.
 */
class ExportSplitTest {
    private val gb = 1024L * 1024 * 1024

    // Releases as sizes, split with the given limit, back as sizes.
    private fun split(sizes: List<Long>, max: Long = MAX_ZIP_BYTES): List<List<Long>> = splitReleases(sizes, max) { it }

    @Test
    fun nothingMakesNoZips() {
        assertEquals(emptyList<List<Long>>(), split(emptyList()))
    }

    @Test
    fun aLibraryThatFitsIsOneZip() {
        assertEquals(listOf(listOf(gb, 2 * gb, 3 * gb)), split(listOf(gb, 2 * gb, 3 * gb)))
    }

    @Test
    fun exactlyTheLimitFits() {
        assertEquals(listOf(listOf(4 * gb, 4 * gb)), split(listOf(4 * gb, 4 * gb)))
        assertEquals(listOf(listOf(4 * gb, 4 * gb), listOf(1L)), split(listOf(4 * gb, 4 * gb, 1)))
    }

    @Test
    fun aReleaseThatDoesntFitStartsTheNextZip() {
        assertEquals(listOf(listOf(5 * gb), listOf(4 * gb, 3 * gb)), split(listOf(5 * gb, 4 * gb, 3 * gb)))
    }

    @Test
    fun theOrderIsKept() {
        // A later small release doesn't jump back to fill an earlier zip.
        assertEquals(listOf(listOf(6L), listOf(5L, 1L)), split(listOf(6, 5, 1), max = 7))
    }

    @Test
    fun aReleaseTooBigForAnyZipGoesAlone() {
        assertEquals(
            listOf(listOf(gb), listOf(9 * gb), listOf(gb)),
            split(listOf(gb, 9 * gb, gb)),
        )
        assertEquals(listOf(listOf(9 * gb), listOf(10 * gb)), split(listOf(9 * gb, 10 * gb)))
    }

    @Test
    fun manyReleasesStayUnderTheLimitAndWhole() {
        val sizes = (1..500).map { (it * 7919L % 1000 + 1) * 30L * 1024 * 1024 } // 30 MB to 30 GB-ish, mixed
        val zips = split(sizes)
        assertEquals(sizes, zips.flatten())
        for (zip in zips) assertTrue(zip.size == 1 || zip.sum() <= MAX_ZIP_BYTES)
        // Greedy in order: no zip could have taken the next one's first release.
        for ((a, b) in zips.zipWithNext()) assertTrue(a.sum() + b.first() > MAX_ZIP_BYTES)
    }

    @Test
    fun releasesAreSizedByAllTheirFiles() {
        fun release(vararg sizes: Long) = ExportRelease(sizes.mapIndexed { i, n -> ExportFile(File("$i"), "$i", n) })
        val album = release(3 * gb, 3 * gb, 1024) // tracks and a cover
        val next = release(2 * gb)
        val zips = splitReleases(listOf(album, next), MAX_ZIP_BYTES) { it.bytes }
        assertEquals(listOf(listOf(album), listOf(next)), zips)
    }
}
