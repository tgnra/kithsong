package ng.kithso.android.cast

import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class CastPauseTest {
    @Test
    fun threeMinutesPausedEndsTheCast() = runTest {
        val paused = MutableStateFlow(false)
        var ends = 0
        val job = launch { endAfterPaused(paused) { ends++ } }
        runCurrent()
        paused.value = true
        advanceTimeBy(CAST_PAUSED_MS - 1)
        runCurrent()
        assertEquals(0, ends)
        advanceTimeBy(1)
        runCurrent()
        assertEquals(1, ends)
        // Once for each pause, however long it goes on.
        advanceTimeBy(CAST_PAUSED_MS * 3)
        runCurrent()
        assertEquals(1, ends)
        job.cancel()
    }

    @Test
    fun playingAgainStartsTheWaitOver() = runTest {
        val paused = MutableStateFlow(true)
        var ends = 0
        val job = launch { endAfterPaused(paused) { ends++ } }
        advanceTimeBy(CAST_PAUSED_MS - 1)
        runCurrent()
        paused.value = false
        runCurrent()
        advanceTimeBy(CAST_PAUSED_MS * 2)
        runCurrent()
        assertEquals(0, ends)
        // Paused again, it's three minutes from then.
        paused.value = true
        advanceTimeBy(CAST_PAUSED_MS - 1)
        runCurrent()
        assertEquals(0, ends)
        advanceTimeBy(1)
        runCurrent()
        assertEquals(1, ends)
        job.cancel()
    }

    @Test
    fun playingNeverEnds() = runTest {
        val paused = MutableStateFlow(false)
        var ends = 0
        val job = launch { endAfterPaused(paused) { ends++ } }
        advanceTimeBy(CAST_PAUSED_MS * 10)
        runCurrent()
        assertEquals(0, ends)
        job.cancel()
    }

    @Test
    fun aMomentaryPauseDoesNothing() = runTest {
        val paused = MutableStateFlow(false)
        var ends = 0
        val job = launch { endAfterPaused(paused) { ends++ } }
        runCurrent()
        // Paused for a second each minute: never three minutes on end.
        repeat(10) {
            paused.value = true
            advanceTimeBy(1_000)
            runCurrent()
            paused.value = false
            advanceTimeBy(60_000)
            runCurrent()
        }
        assertEquals(0, ends)
        job.cancel()
    }
}
