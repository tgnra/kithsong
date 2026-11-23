package ng.kithso.android.ui

import ng.kithso.android.sync.SyncProgress
import ng.kithso.android.sync.SyncStatus
import ng.kithso.core.CoreSync
import ng.kithso.core.DeviceKind
import ng.kithso.core.SyncActivity
import ng.kithso.core.SyncDevice
import ng.kithso.core.SyncOutlook
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SyncPillTest {
    private val member = SyncDevice(
        "desk", "Desk", DeviceKind.Desktop.ordinal, isSelf = false, online = true, archive = true, removed = false,
        address = null, port = 0, lastSyncMs = 0, autoSyncAfterMs = 0, hasStatus = false, verifiedMs = 0,
        expected = 0, ok = 0, missing = 0, corrupt = 0, bytes = 0,
    )
    private val upToDate = SyncOutlook(true, 0, 0, 0, 0, 0, "Up to date")
    private val toFetch = SyncOutlook(true, 3, 30_000_000, 0, 0, 0, "3 tracks to fetch")
    private val group = SyncStatus(
        "Phone",
        running = true,
        libraryState = CoreSync.LibraryState.Group,
        members = listOf(member),
        outlook = upToDate,
    )

    @Test
    fun idleShowsTheOutlookAndSyncNow() {
        val state = pillState(group) as PillState.Idle
        assertEquals(upToDate, state.outlook)
        assertFalse(state.needsChoice)
        assertTrue(state.canSync)
    }

    @Test
    fun aJoinedPhoneAsksForItsChoiceFirst() {
        val state = pillState(group.copy(outlook = upToDate.copy(chosen = false, text = ""))) as PillState.Idle
        assertTrue(state.needsChoice)
        assertTrue(state.canSync) // pressing Sync now is the choice
    }

    @Test
    fun syncOffCantSync() {
        val state = pillState(group.copy(running = false)) as PillState.Idle
        assertNull(state.outlook)
        assertFalse(state.canSync)
    }

    @Test
    fun syncOffStillTakesAPressToShake() {
        assertTrue((pillState(group.copy(running = false)) as PillState.Idle).blocked)
    }

    @Test
    fun nothingWrongNoReason() {
        val state = pillState(group.copy(outlook = toFetch)) as PillState.Idle
        assertNull(state.reason)
        assertFalse(state.upToDate)
        assertFalse(state.blocked)
    }

    @Test
    fun upToDateGoesAheadAndShowsTheLastSync() {
        val state = pillState(group.copy(activity = SyncActivity(1, 100, 4, 0, "Last sync: ↑ 4 · ↓ 100"))) as PillState.Idle
        assertTrue(state.upToDate)
        assertFalse(state.blocked) // it shakes only once the sessions found nothing to do
        assertTrue(state.canSync) // playlists and plays aren't in the outlook
        assertEquals("Last sync: ↑ 4 · ↓ 100", state.activity)
    }

    @Test
    fun deletesWaitingAreSomethingToDo() {
        assertFalse((pillState(group.copy(binCount = 2)) as PillState.Idle).upToDate)
    }

    @Test
    fun tracksStillMissingAreSomethingToDo() {
        assertFalse((pillState(group.copy(finished = "Finished, but 3 tracks are still missing")) as PillState.Idle).upToDate)
    }

    @Test
    fun offWifiComesFirst() {
        val state = pillState(group.copy(onLan = false, cancelled = true)) as PillState.Idle
        assertEquals(PillReason.NotOnLan, state.reason)
        assertTrue(state.blocked)
    }

    @Test
    fun cancelledSaysSoButSyncNowGoesAhead() {
        val state = pillState(group.copy(cancelled = true)) as PillState.Idle
        assertEquals(PillReason.Cancelled, state.reason)
        assertFalse(state.blocked)
    }

    @Test
    fun theOnlyMemberOfflineIsNamed() {
        val state = pillState(group.copy(members = listOf(member.copy(online = false)))) as PillState.Idle
        assertEquals(PillReason.Offline("Desk"), state.reason)
        assertTrue(state.blocked)
        assertTrue(state.canSync) // the press still goes to the core, which may reach it anyway
    }

    @Test
    fun severalOfflineAreNotNamed() {
        val away = member.copy(online = false)
        val state = pillState(group.copy(members = listOf(away, away.copy(uid = "tab", name = "Tab")))) as PillState.Idle
        assertEquals(PillReason.Offline(null), state.reason)
    }

    @Test
    fun aFailedSessionIsShownButNotACancelledOne() {
        val done = CoreSync.Event.SyncDone("desk", CoreSync.ERR_LOCKED, false, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        assertEquals(
            PillReason.Failed("Desk", CoreSync.ERR_LOCKED),
            (pillState(group.copy(results = mapOf("desk" to done))) as PillState.Idle).reason,
        )
        val cancelled = done.copy(cancelled = true)
        assertNull((pillState(group.copy(results = mapOf("desk" to cancelled))) as PillState.Idle).reason)
    }

    @Test
    fun neverProgressWithoutASession() {
        // Transcoding, a pairing, or waiting tracks: still no progress while nothing syncs.
        assertTrue(pillState(group.copy(transcoding = true)) is PillState.Idle)
        assertTrue(pillState(group.copy(sessions = emptyMap())) is PillState.Idle)
    }

    @Test
    fun syncingCountsWhatsLeftBothWays() {
        val status = group.copy(
            transcoding = true,
            sessions = mapOf(
                "desk" to SyncProgress(CoreSync.Phase.Files, filesDone = 3, filesTotal = 10),
                "tab" to SyncProgress(CoreSync.Phase.Files, sendsDone = 1, sendsTotal = 5),
            ),
        )
        val state = pillState(status) as PillState.Syncing
        assertEquals(7, state.receiveLeft)
        assertEquals(4, state.sendLeft)
        assertTrue(state.transcoding)
        assertEquals(4f / 15f, state.fraction!!, 1e-6f)
    }

    @Test
    fun beforeTheFilesTheBarHasNoFraction() {
        val state = pillState(group.copy(sessions = mapOf("desk" to SyncProgress(CoreSync.Phase.Connecting)))) as PillState.Syncing
        assertNull(state.fraction)
        assertEquals(0, state.receiveLeft)
        assertEquals(0, state.sendLeft)
    }

    @Test
    fun finishedWithTracksMissingTakesTheSummarysPlace() {
        val finished = "Finished, but 3 tracks are still missing"
        assertEquals(finished, (pillState(group.copy(finished = finished)) as PillState.Idle).finished)
        assertEquals("", (pillState(group) as PillState.Idle).finished)
    }

    @Test
    fun finishedGivesWayToSyncOffAChoiceOrASession() {
        val finished = group.copy(finished = "Finished, but 1 track is still missing")
        assertEquals("", (pillState(finished.copy(running = false)) as PillState.Idle).finished)
        assertEquals("", (pillState(finished.copy(outlook = upToDate.copy(chosen = false))) as PillState.Idle).finished)
        assertTrue(pillState(finished.copy(sessions = mapOf("desk" to SyncProgress(CoreSync.Phase.Files)))) is PillState.Syncing)
    }

    @Test
    fun oneSessionShowsItsSpeedAndTimeLeft() {
        val pace = "2.1 MB/s · about 4 min left"
        val status = group.copy(sessions = mapOf("desk" to SyncProgress(CoreSync.Phase.Files, filesTotal = 9, pace = pace)))
        assertEquals(pace, (pillState(status) as PillState.Syncing).pace)
    }

    @Test
    fun noSpeedUntilTheCoreHasOne() {
        // The core leaves it out until it has settled, in a stall, and before the files.
        val status = group.copy(sessions = mapOf("desk" to SyncProgress(CoreSync.Phase.Files, filesTotal = 9)))
        assertEquals("", (pillState(status) as PillState.Syncing).pace)
        assertEquals("", pillPace(emptyList()))
    }

    @Test
    fun severalSessionsShowNoSpeed() {
        val sessions = listOf(
            SyncProgress(CoreSync.Phase.Files, filesTotal = 9, pace = "2.1 MB/s"),
            SyncProgress(CoreSync.Phase.Files, sendsTotal = 3, pace = "0.4 MB/s · less than a minute left"),
        )
        assertEquals("", pillPace(sessions))
    }
}
