package ng.kithso.android.ui

import ng.kithso.core.DeviceKind
import ng.kithso.core.SyncDevice
import ng.kithso.core.TranscodeTarget
import org.junit.Assert.assertEquals
import org.junit.Test

class TranscodeTargetsTest {
    private fun member(uid: String, name: String) = SyncDevice(
        uid, name, DeviceKind.Desktop.ordinal, isSelf = false, online = true, archive = false, removed = false,
        address = null, port = 0, lastSyncMs = 0, autoSyncAfterMs = 0, hasStatus = false, verifiedMs = 0,
        expected = 0, ok = 0, missing = 0, corrupt = 0, bytes = 0,
    )

    @Test
    fun namedByTheMembersInTheirOrder() {
        val members = listOf(member("desk", "Desk"), member("tab", "Tab"))
        assertEquals(
            listOf(3 to "Tab", 40 to "Desk"),
            transcodeTargets(listOf(TranscodeTarget("tab", 3), TranscodeTarget("desk", 40)), members),
        )
    }

    @Test
    fun unsaidAndUnknownAreLeftOut() {
        val members = listOf(member("desk", "Desk"))
        assertEquals(
            listOf(5 to "Desk"),
            transcodeTargets(listOf(TranscodeTarget("", 2), TranscodeTarget("gone", 1), TranscodeTarget("desk", 5)), members),
        )
    }
}
