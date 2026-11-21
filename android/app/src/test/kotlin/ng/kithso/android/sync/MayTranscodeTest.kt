package ng.kithso.android.sync

import ng.kithso.android.sync.Syncer.Companion.HOLDER_APP
import ng.kithso.android.sync.Syncer.Companion.HOLDER_BACKGROUND
import ng.kithso.android.sync.Syncer.Companion.HOLDER_VERIFY
import ng.kithso.android.sync.Syncer.Companion.mayTranscode
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MayTranscodeTest {
    private val service = "service"

    @Test
    fun chargingAlwaysMay() {
        assertTrue(mayTranscode(charging = true, syncScreenShown = false, holders = emptySet()))
        // The app gone, a session kept going by the service: it carries on.
        assertTrue(mayTranscode(charging = true, syncScreenShown = false, holders = setOf(service)))
    }

    @Test
    fun onBatteryOnlyWithASyncScreenOnScreen() {
        assertTrue(mayTranscode(charging = false, syncScreenShown = true, holders = setOf(HOLDER_APP)))
        assertFalse(mayTranscode(charging = false, syncScreenShown = false, holders = setOf(HOLDER_APP, service)))
        // Shown but the app no longer holds sync: it has left the screen.
        assertFalse(mayTranscode(charging = false, syncScreenShown = true, holders = setOf(service)))
        assertFalse(mayTranscode(charging = false, syncScreenShown = false, holders = setOf(HOLDER_VERIFY)))
    }

    @Test
    fun backgroundMayOnBatteryOffScreen() {
        assertTrue(mayTranscode(charging = false, syncScreenShown = false, holders = setOf(HOLDER_BACKGROUND, service)))
        assertTrue(mayTranscode(charging = false, syncScreenShown = false, holders = setOf(HOLDER_BACKGROUND)))
    }

    @Test
    fun backgroundKeepsTheServiceBusy() {
        val status = SyncStatus("Phone")
        assertFalse(status.busy)
        assertTrue(status.copy(background = true).busy)
        // Transcoding on battery shows the notice, with Background or saying it's in the background.
        assertTrue(status.copy(transcoding = true).transcodingOnBattery)
        assertFalse(status.copy(transcoding = true, charging = true).transcodingOnBattery)
    }
}
