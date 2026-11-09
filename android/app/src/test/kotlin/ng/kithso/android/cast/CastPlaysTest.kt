package ng.kithso.android.cast

import ng.kithso.core.CorePlayer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CastPlaysTest {
    private var mono = 0L
    private var wall = 1_000_000L
    private val nowPlaying = mutableListOf<String>()
    private val played = mutableListOf<Pair<String, Long>>()
    private val plays = CastPlays({ nowPlaying += it }, { uid, started -> played += uid to started }, { wall }, { mono })

    private fun pass(ms: Long) {
        mono += ms
        wall += ms
    }

    private fun row(uid: String, end: CorePlayer.PlayEnd = CorePlayer.PlayEnd.Skipped) =
        CorePlayer.PlayEvent(uid, 5, 0, end, CorePlayer.Origin(CorePlayer.PlayContext.Album, "album"))

    @Test
    fun theCoresRule() {
        assertFalse(playCounts(200_000, 99_999))
        assertTrue(playCounts(200_000, 100_000))
        // Four minutes for a long track, or one whose length isn't known.
        assertFalse(playCounts(600_000, 239_999))
        assertTrue(playCounts(600_000, 240_000))
        assertTrue(playCounts(0, 240_000))
        assertFalse(playCounts(0, 239_999))
    }

    @Test
    fun aPlayCountsByTheTimeItPlayed() {
        plays.casting = true
        plays.start("a", 200_000)
        pass(5_000) // loading
        plays.update(playing = true)
        val began = wall
        assertEquals(listOf("a"), nowPlaying)
        pass(60_000)
        plays.update(playing = false) // paused
        pass(600_000)
        plays.update(playing = true)
        pass(39_999)
        plays.update(playing = true)
        assertTrue(played.isEmpty())
        pass(1)
        plays.update(playing = true)
        assertEquals(listOf("a" to began), played)
        // Once.
        pass(100_000)
        plays.update(playing = true)
        assertEquals(1, played.size)
        // Announced again after the pause, as the phone does.
        assertEquals(listOf("a", "a"), nowPlaying)
    }

    @Test
    fun theCoresRowGetsWhatTheDevicePlayed() {
        plays.casting = true
        plays.start("a", 200_000)
        plays.update(playing = true)
        val began = wall
        pass(30_000)
        plays.end(CorePlayer.PlayEnd.Finished)
        val row = plays.patch(row("a"))!!
        assertEquals(30_000, row.listenedMs)
        assertEquals(began, row.startedMs)
        assertEquals(CorePlayer.PlayEnd.Finished, row.end)
        assertEquals(CorePlayer.PlayContext.Album, row.origin.context)
        // Taken once.
        assertNull(plays.patch(row("a")))
    }

    @Test
    fun rowsTheDeviceDidntPlayGo() {
        plays.casting = true
        assertNull(plays.patch(row("b")))
        // But a file the core couldn't open is still logged.
        val failed = row("c", CorePlayer.PlayEnd.Failed)
        assertEquals(failed, plays.patch(failed))
    }

    @Test
    fun notCastingRowsStayAsTheyAre() {
        val local = row("a", CorePlayer.PlayEnd.Finished).copy(listenedMs = 12_345)
        assertEquals(local, plays.patch(local))
    }

    @Test
    fun aPlaySkippedThroughWithNoRowIsPassedOver() {
        plays.casting = true
        plays.start("a", 200_000)
        plays.end(CorePlayer.PlayEnd.Skipped)
        plays.start("b", 200_000)
        plays.update(playing = true)
        pass(10_000)
        plays.end(CorePlayer.PlayEnd.Skipped)
        assertEquals(10_000, plays.patch(row("b"))!!.listenedMs)
        assertNull(plays.patch(row("a")))
    }
}
