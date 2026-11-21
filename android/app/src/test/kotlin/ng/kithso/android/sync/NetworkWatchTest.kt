package ng.kithso.android.sync

import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class NetworkWatchTest {
    @Test
    fun aBurstRunsOnceASecondAfterItSettles() = runTest {
        var runs = 0
        val debouncer = Debouncer(this, StandardTestDispatcher(testScheduler), 1_000) { runs++ }
        repeat(5) {
            debouncer.poke()
            advanceTimeBy(300)
        }
        assertEquals(0, runs)
        advanceTimeBy(701)
        runCurrent()
        assertEquals(1, runs)
        debouncer.poke()
        advanceTimeBy(1_001)
        runCurrent()
        assertEquals(2, runs)
    }

    @Test
    fun cancelledRunsNothing() = runTest {
        var runs = 0
        val debouncer = Debouncer(this, StandardTestDispatcher(testScheduler), 1_000) { runs++ }
        debouncer.poke()
        debouncer.cancel()
        advanceTimeBy(2_000)
        runCurrent()
        assertEquals(0, runs)
    }

    @Test
    fun theNetworksThereOnRegisteringChangeNothing() {
        val changes = NetworkChanges("192.168.1.5")
        assertFalse(changes.settle("192.168.1.5"))
    }

    @Test
    fun anotherAddressOrALossIsAChange() {
        val changes = NetworkChanges("192.168.1.5")
        assertTrue(changes.settle("10.0.0.7"))
        assertFalse(changes.settle("10.0.0.7"))
        // Wi-Fi dropped and came back with the same address: the sockets may be dead.
        changes.lost()
        assertTrue(changes.settle("10.0.0.7"))
        assertFalse(changes.settle("10.0.0.7"))
        assertTrue(changes.settle(null))
    }
}
