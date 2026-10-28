package ng.kithso.wear

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.delay
import kotlin.random.Random

/**
 * What the sync screen shows. The watch is always device B in pairing (see "Pairing" in
 * docs/sync-protocol.md): another device asks to pair, the watch shows the code, and the
 * other device's user types it in. What the watch holds is its sync profile, which is edited
 * on the other devices in the group.
 */
sealed interface SyncState {
    /** Discoverable and waiting for another device to start pairing. */
    data class Waiting(val deviceName: String) : SyncState

    /** [peerName] asked to pair. [attemptsLeft] drops below 3 after a wrong code. */
    data class ShowingCode(val peerName: String, val code: String, val attemptsLeft: Int) :
        SyncState

    /** The watch refused [peerName]'s pairing request. */
    data class Refused(val peerName: String, val reason: RefuseReason) : SyncState

    /** In a group with [devices]; [synced] of [wanted] tracks are on the watch. */
    data class Paired(val devices: List<String>, val synced: Int, val wanted: Int) : SyncState
}

/** The join-rule refusals the watch can hit as device B. */
enum class RefuseReason { InGroup, LibraryMismatch, AlreadyPaired }

/**
 * Stands in for musiccore's sync until :wear depends on :native again. [run] plays out a
 * pairing request from a made-up desktop, then a sync of every sample track.
 */
class SampleSync(private val deviceName: String, private val trackCount: Int) {
    var state by mutableStateOf<SyncState>(SyncState.Waiting(deviceName))
        private set

    /** Listens while the sync screen is open; cancelled when it closes. */
    suspend fun run() {
        try {
            if (state !is SyncState.Paired) {
                state = SyncState.Waiting(deviceName)
                delay(3_000)
                state = SyncState.ShowingCode(PEER, "%06d".format(Random.nextInt(1_000_000)), 3)
                delay(8_000)
            }
            val from = (state as? SyncState.Paired)?.synced ?: 0
            for (synced in from..trackCount) {
                state = SyncState.Paired(listOf(PEER), synced, trackCount)
                delay(250)
            }
        } finally {
            // Leaving the screen abandons a pairing in progress.
            if (state is SyncState.ShowingCode) state = SyncState.Waiting(deviceName)
        }
    }

    fun dismissRefusal() {
        if (state is SyncState.Refused) state = SyncState.Waiting(deviceName)
    }

    private companion object {
        const val PEER = "Linux desktop"
    }
}
