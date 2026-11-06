package ng.kithso.wear

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.Immutable
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.core.ConfirmState
import ng.kithso.core.CoreSync
import ng.kithso.core.DeviceKind
import ng.kithso.core.KithCoreException
import ng.kithso.core.SyncDevice
import ng.kithso.core.SyncWaiting
import java.io.File

/**
 * What the sync screen shows. The watch is always device B in pairing (see "Pairing" in
 * docs/sync-protocol.md): another device asks to pair, the watch shows the code, and the
 * other device's user types it in. What the watch holds is its sync profile, which is chosen
 * and confirmed on the other devices in the group: the watch has no editor and no Sync button.
 */
@Immutable
sealed interface SyncState {
    /** Discoverable and waiting for another device to start pairing. */
    data class Waiting(val deviceName: String) : SyncState

    /** Sync couldn't start, so the watch can't be found or paired. */
    data class Unavailable(val error: SyncError) : SyncState

    /** [peerName] asked to pair. [attemptsLeft] drops below 3 after a wrong code. */
    data class ShowingCode(val peerName: String, val code: String, val attemptsLeft: Int) : SyncState

    /** The watch refused [peerName]'s pairing request, or it failed. */
    data class Refused(val peerName: String, val reason: RefuseReason) : SyncState

    /**
     * In a group with [devices] (the other members' names, "" for one with none). [confirm] is
     * whether this watch's profile is confirmed: until it is, nothing is copied to it. With
     * [hasStatus], [synced] of [wanted] files are here as the watch last reported them to the
     * group; before its first file phase, [synced] counts the tracks with a file here and
     * [wanted] is 0. [progress] is a session fetching files, [waiting] the tracks that can only
     * come from members the watch can't reach, [problems] what's wrong with the files here as
     * the core words it, and [failure] the last session that failed.
     */
    data class Paired(
        val devices: List<String>,
        val confirm: ConfirmState,
        val synced: Int,
        val wanted: Int,
        val hasStatus: Boolean,
        val progress: FileProgress? = null,
        val waiting: SyncWaiting = SyncWaiting.NONE,
        val problems: String = "",
        val failure: SyncFailure? = null,
    ) : SyncState
}

/** The join-rule refusals the watch can hit as device B, and a pairing that broke down. */
enum class RefuseReason { InGroup, LibraryMismatch, AlreadyPaired, Failed }

/** Why sync couldn't start. */
enum class SyncError { Open, Listen }

/** Files fetched so far of those a session brings: [done] of [total]. */
@Immutable
data class FileProgress(val done: Int, val total: Int, val pace: String = "")

/** A session with [deviceName] that failed with [status], a negative mc_status. */
@Immutable
data class SyncFailure(val deviceName: String, val status: Int)

/** An incoming pairing, as far as it has got. */
@Immutable
sealed interface Pairing {
    /** [peer] asked to pair: its user types [code] in there. [attemptsLeft] drops after a wrong code. */
    data class ShowCode(val peer: String, val code: String, val attemptsLeft: Int) : Pairing

    /** It ended without pairing, and the screen says why until dismissed. */
    data class Refused(val peer: String, val reason: RefuseReason) : Pairing
}

/** Sync as [WatchSyncer] has it; [screen] is the sync screen's view of it. */
@Immutable
data class SyncStatus(
    /** What other devices call this watch. */
    val deviceName: String,
    /** Listening, discovering and syncing; only while something holds it (see [WatchSyncer.hold]). */
    val running: Boolean = false,
    /** Why sync couldn't start, if it couldn't. */
    val error: SyncError? = null,
    val inGroup: Boolean = false,
    /** This watch as a member of the group, for its confirm state and file status; null outside one. */
    val self: SyncDevice? = null,
    /** The other members of the group, removed ones left out, by name. */
    val members: List<SyncDevice> = emptyList(),
    /** The tracks with a file here, from the library. */
    val tracksHere: Int = 0,
    /** The sessions running, by member uid, with their files so far. */
    val sessions: Map<String, FileProgress> = emptyMap(),
    /** The last session that failed, until one succeeds. */
    val failure: SyncFailure? = null,
    /** Tracks waiting for a member this watch can't reach now. */
    val waiting: SyncWaiting = SyncWaiting.NONE,
    /** What's wrong with the files here, as the core words it; empty if nothing is. */
    val problems: String = "",
    val pairing: Pairing? = null,
) {
    /** Something is under way that shouldn't be cut off: a session or a pairing. */
    val busy: Boolean get() = sessions.isNotEmpty() || pairing is Pairing.ShowCode

    /** Files being fetched in every session at once (usually just one), or null while none are. */
    val filesProgress: FileProgress? get() {
        val files = sessions.values.filter { it.total > 0 }
        if (files.isEmpty()) return null
        return FileProgress(
            files.sumOf { it.done.coerceAtMost(it.total) },
            files.sumOf { it.total },
            files.singleOrNull()?.pace.orEmpty(),
        )
    }

    val screen: SyncState get() = when {
        pairing is Pairing.ShowCode -> SyncState.ShowingCode(pairing.peer, pairing.code, pairing.attemptsLeft)
        pairing is Pairing.Refused -> SyncState.Refused(pairing.peer, pairing.reason)
        inGroup -> {
            val status = self?.takeIf { it.hasStatus }
            SyncState.Paired(
                devices = members.map { it.name },
                confirm = self?.confirm ?: ConfirmState.Unconfirmed,
                synced = status?.ok?.toInt() ?: tracksHere,
                wanted = status?.expected?.toInt() ?: 0,
                hasStatus = status != null,
                progress = filesProgress,
                waiting = waiting,
                problems = problems,
                failure = failure,
            )
        }
        error != null -> SyncState.Unavailable(error)
        else -> SyncState.Waiting(deviceName)
    }
}

/**
 * LAN sync with the other devices in this watch's group, through the core ([CoreSync]): the
 * phone's Syncer, cut down to what a watch does. It belongs to the process, not the UI. The
 * core listens, discovers and syncs on its own threads while sync runs, and sync runs while
 * something [hold]s it: the app on screen, [SyncService] while a session or pairing is under
 * way, and [SyncWorker] for the hourly background run on the charger.
 *
 * Sessions start by themselves while the app is on screen or the background run holds sync,
 * as the core schedules them (a member synced completely within the hour waits for the hour).
 * Otherwise the core starts nothing new ([CoreSync.setAutomatic]), though it still serves a
 * session another device starts, and sync stops as soon as it's idle.
 *
 * Wear OS keeps Wi-Fi off while the phone is connected over Bluetooth, and the other devices
 * are only reachable over Wi-Fi, so a Wi-Fi network is requested while sync runs
 * ([WifiRequest]), and the core's multicast lock is held so discovery hears the mDNS answers.
 *
 * The watch only ever pairs as device B: it shows the code, and never asks. It offers no
 * library merging (see [CoreSync]'s offerMerge): pairing with a device whose library has
 * music, while the watch's has too, is refused, and a merge proposal, should one come anyway,
 * is declined. Its profile is edited and confirmed on the other devices, so sessions here are
 * started with [CoreSync.syncNow] and [CoreSync.syncBackground], never confirmSyncNow.
 *
 * Synced data and audio files land in [library]'s folder, which is read again after every
 * session that brought or removed something, and now and then while files arrive. This watch
 * starts out as [deviceName]; renamed by another member, it takes on the new name and passes
 * it to [onRenamed], to be saved and given as [deviceName] next time.
 *
 * Every call on the core runs on one thread ([thread]); the core's events arrive on its
 * threads and are queued over to that one.
 */
class WatchSyncer(
    private val context: Context,
    private val library: WatchLibrary,
    private val configDir: File,
    deviceName: String,
    private val scope: CoroutineScope,
    private val onRenamed: (String) -> Unit = {},
) {
    private val thread = Dispatchers.IO.limitedParallelism(1)
    private val state = MutableStateFlow(SyncStatus(deviceName))
    val status: StateFlow<SyncStatus> = state

    /** The sync screen's view of [status]. */
    val screen: StateFlow<SyncState> = state.map { it.screen }.stateIn(scope, SharingStarted.Eagerly, state.value.screen)

    // Only touched on [thread].
    private var sync: CoreSync? = null
    private val holders = HashSet<String>()
    private val multicastLock: WifiManager.MulticastLock? by lazy {
        // Without it Android drops incoming multicast, and discovery (mDNS) hears nothing.
        context.applicationContext.getSystemService(WifiManager::class.java)
            ?.createMulticastLock("kithsong:sync")
            ?.apply { setReferenceCounted(false) }
    }
    private val wifi = WifiRequest(context.applicationContext)
    private val events = Channel<CoreSync.Event>(Channel.UNLIMITED)

    // When the library was last read again for files that arrived (elapsed realtime). Only
    // touched on [thread].
    private var lastFilesRefresh = 0L

    init {
        scope.launch(thread) {
            for (event in events) onEvent(event)
        }
        scope.launch(thread) {
            library.contents.collect { contents -> update { copy(tracksHere = contents.trackCount) } }
        }
    }

    /**
     * Keeps sync running until [release] by the same [holder]. [HOLDER_APP] (the app on
     * screen) and [HOLDER_WORKER] (the background run) let it start sessions by itself.
     */
    fun hold(holder: String) {
        scope.launch(thread) {
            holders += holder
            val sync = sync
            if (!state.value.running) {
                start()
            } else if (sync != null) {
                sync.setAutomatic(automatic())
                refreshLists(sync)
            }
        }
    }

    fun release(holder: String) {
        scope.launch(thread) {
            holders -= holder
            // Off the screen: what's under way may finish, but nothing new starts.
            sync?.setAutomatic(automatic())
            stopIfIdle()
        }
    }

    /**
     * Asks for a session with every member that can be reached, for the background run: a
     * member synced completely within the hour gets none (see [SyncWorker]).
     */
    fun syncBackground() = call { it.syncBackground() }

    /**
     * Goes on with the weekly deep check for up to [budgetMs], and returns true once it's over
     * for this week, or can't go on. Off [thread], as the core holds nothing up while it reads,
     * so call it only while holding sync.
     */
    suspend fun verifyWeekly(budgetMs: Long): Boolean {
        val sync = withContext(thread) { sync } ?: return true
        return withContext(Dispatchers.IO) {
            try {
                sync.verifyWeekly(budgetMs)
            } catch (e: KithCoreException) {
                Log.w(TAG, "Weekly check failed", e)
                true
            }
        }
    }

    /** Gives up the pairing in progress, from the service's timeout. */
    fun cancelPairing() = call { it.pairCancel() }

    /** Forgets why the last pairing was refused, once it has been seen. */
    fun dismissRefusal() {
        scope.launch(thread) {
            if (state.value.pairing is Pairing.Refused) update { copy(pairing = null) }
        }
    }

    /** The library changed here, so the other devices should hear of it soon. */
    fun notifyChanged() {
        call { it.notifyChanged() }
    }

    // On [thread]. Whether the core may start sessions by itself.
    private fun automatic() = HOLDER_APP in holders || HOLDER_WORKER in holders

    // On [thread]: keeps the process going until a session or pairing is over, should the app
    // leave the screen meanwhile. Only started while the app is on screen: from the background
    // Android won't start it, and the background run holds sync itself.
    private fun startService() {
        if (HOLDER_APP in holders) SyncService.start(context)
    }

    // Runs [block] on [thread] with the running sync, logging the core's failures.
    private fun call(block: (CoreSync) -> Unit) {
        scope.launch(thread) {
            val sync = sync?.takeIf { state.value.running } ?: return@launch
            try {
                block(sync)
            } catch (e: KithCoreException) {
                Log.w(TAG, "Sync call failed", e)
            }
        }
    }

    private inline fun update(change: SyncStatus.() -> SyncStatus) {
        state.value = state.value.change()
    }

    // On [thread].
    private suspend fun start() {
        val sync = sync ?: try {
            configDir.mkdirs()
            library.onCore { lib ->
                CoreSync(lib, configDir.path, state.value.deviceName, DeviceKind.Watch, { events.trySend(it) }, offerMerge = false)
            }
        } catch (e: KithCoreException) {
            Log.e(TAG, "Couldn't open sync", e)
            null
        }.also { sync = it }
        if (sync == null) {
            update { copy(error = SyncError.Open) }
            return
        }
        wifi.request()
        multicastLock?.acquire()
        sync.setAutomatic(automatic())
        try {
            sync.start()
        } catch (e: KithCoreException) {
            Log.e(TAG, "Couldn't start sync", e)
            multicastLock?.release()
            wifi.release()
            update { copy(error = SyncError.Listen) }
            return
        }
        Log.i(TAG, "Sync running on port ${sync.port}")
        update { copy(running = true, error = null) }
        refreshLists(sync)
    }

    // On [thread]. Stops once nothing holds sync: without a holder the process may be frozen
    // or cut off from the network at any moment, so it's ended rather than left hanging.
    private fun stopIfIdle() {
        val sync = sync ?: return
        if (holders.isNotEmpty() || !state.value.running) return
        sync.stop()
        multicastLock?.release()
        wifi.release()
        update { copy(running = false, sessions = emptyMap()) }
        Log.i(TAG, "Sync stopped")
    }

    // On [thread].
    private fun onEvent(event: CoreSync.Event) {
        val sync = sync ?: return
        when (event) {
            is CoreSync.Event.Peer -> Unit
            // Read by refreshLists below.
            CoreSync.Event.Waiting -> Unit
            is CoreSync.Event.PairCode -> {
                val peer = event.requester.ifEmpty { context.getString(R.string.sync_other_device) }
                update { copy(pairing = Pairing.ShowCode(peer, event.code, event.attemptsLeft)) }
                startService()
            }
            is CoreSync.Event.PairResult -> onPairResult(event)
            is CoreSync.Event.PairMerge -> {
                // The watch offers no merging, so none should be proposed; one that is anyway
                // is declined, which ends the pairing as a mismatch would.
                Log.w(TAG, "Pairing with ${event.name} proposed merging libraries: declined")
                try {
                    if (event.needsConfirm) sync.pairMergeConfirm(false) else sync.pairCancel()
                } catch (e: KithCoreException) {
                    Log.w(TAG, "Couldn't decline the merge", e)
                }
                return
            }
            is CoreSync.Event.MergeProgress, is CoreSync.Event.MergeDone -> {
                Log.w(TAG, "Unexpected merge event on the watch: $event")
                return
            }
            is CoreSync.Event.Progress -> if (!onProgress(event)) return
            is CoreSync.Event.SyncDone -> {
                Log.i(
                    TAG,
                    "Synced with ${event.uid}: status ${event.status}, ${event.rowsReceived} rows in, " +
                        "${event.rowsSent} out, ${event.filesReceived} files in (${event.bytesReceived} bytes), " +
                        "${event.filesSent} out, ${event.filesDeleted} deleted, ${event.filesMissing} missing, " +
                        "${event.filesCorrupt} corrupt, ${event.coversReceived} covers in",
                )
                // An automatic attempt the member never properly answered: nothing was shown
                // for it, so its failure isn't either.
                if (!event.user && !event.connected) return
                val failure = if (event.status != 0 && !event.cancelled) SyncFailure(memberName(event.uid), event.status) else null
                update { copy(sessions = sessions - event.uid, failure = failure) }
                // Albums and playlists from the other devices, and the files and covers that
                // arrived or went. Files that arrived before a session failed are kept, so they
                // count either way.
                val rows = event.status == 0 && event.rowsReceived > 0
                val files = event.filesReceived > 0 || event.filesDeleted > 0 || event.filesUnknown > 0
                if (rows || files || event.coversReceived > 0) {
                    library.refresh()
                    lastFilesRefresh = SystemClock.elapsedRealtime()
                }
            }
        }
        refreshLists(sync)
        stopIfIdle()
    }

    // On [thread]. Tracks a session's progress; true if the session just started or ended,
    // so the lists should be read again. Most events are a file further on, so they don't.
    private fun onProgress(event: CoreSync.Event.Progress): Boolean {
        val before = state.value.sessions[event.uid]
        if (event.phase == CoreSync.Phase.Done) {
            update { copy(sessions = sessions - event.uid) }
            // Nothing to read again after an attempt that was never shown.
            return before != null
        }
        // An automatic attempt (the core trying members now and then, whether or not they're
        // there) shows nothing until the member answers; one the user asked for shows at once.
        if (!event.user && !event.connected) return false
        val files = event.phase == CoreSync.Phase.Files
        val progress = if (files) FileProgress(event.filesDone, event.filesTotal, event.pace) else before ?: FileProgress(0, 0)
        update { copy(sessions = sessions + (event.uid to progress)) }
        // Show what has arrived so far on a long transfer, rather than only at the end.
        val now = SystemClock.elapsedRealtime()
        if (files && progress.done > (before?.done ?: 0) && now - lastFilesRefresh >= FILES_REFRESH_MS) {
            lastFilesRefresh = now
            library.refresh()
        }
        if (before != null) return false
        startService()
        return true
    }

    // On [thread]. The watch is device B, so the result is of a pairing another device asked
    // for: paired, the group view takes over; refused, the screen says why until dismissed.
    private fun onPairResult(event: CoreSync.Event.PairResult) {
        val shown = state.value.pairing as? Pairing.ShowCode
        val peer = event.name.ifEmpty { shown?.peer ?: context.getString(R.string.sync_other_device) }
        Log.i(TAG, "Pairing with $peer: ${event.reason}, status ${event.status}")
        val next = when (event.reason) {
            // The other device's user typed the wrong code: keep showing it (PairCode brings
            // a new one after too many tries).
            CoreSync.PairReason.WrongCode -> shown?.copy(attemptsLeft = event.attemptsLeft)
            // The other device gave up, or is trying another device first.
            CoreSync.PairReason.Cancelled, CoreSync.PairReason.Busy -> null
            CoreSync.PairReason.Paired -> {
                library.refresh()
                null
            }
            CoreSync.PairReason.InGroup -> Pairing.Refused(peer, RefuseReason.InGroup)
            CoreSync.PairReason.LibraryMismatch, CoreSync.PairReason.MergeDeclined ->
                Pairing.Refused(peer, RefuseReason.LibraryMismatch)
            CoreSync.PairReason.AlreadyPaired -> Pairing.Refused(peer, RefuseReason.AlreadyPaired)
            CoreSync.PairReason.Failed -> Pairing.Refused(peer, RefuseReason.Failed)
        }
        update { copy(pairing = next) }
    }

    // On [thread]. Reads the group again.
    private fun refreshLists(sync: CoreSync) {
        try {
            val devices = sync.devices()
            val inGroup = sync.libraryState != CoreSync.LibraryState.NoGroup
            val waiting = sync.waiting()
            val problems = sync.problems().text
            // Renamed by another member in a session.
            val name = sync.deviceName
            if (name.isNotEmpty() && name != state.value.deviceName) onRenamed(name)
            update {
                copy(
                    deviceName = name.ifEmpty { deviceName },
                    inGroup = inGroup,
                    self = devices.firstOrNull { it.isSelf },
                    members = devices.filter { !it.isSelf && !it.removed },
                    waiting = waiting,
                    problems = problems,
                )
            }
        } catch (e: KithCoreException) {
            Log.e(TAG, "Couldn't read the sync group", e)
        }
    }

    private fun memberName(uid: String): String =
        state.value.members.firstOrNull { it.uid == uid }?.name?.ifEmpty { null }
            ?: context.getString(R.string.sync_other_device)

    companion object {
        /** What the app holds sync as while it's on screen. */
        const val HOLDER_APP = "app"

        /** What [SyncWorker] holds sync as during a background run. */
        const val HOLDER_WORKER = "worker"

        private const val TAG = "kithsong"
        private const val FILES_REFRESH_MS = 10_000L
    }
}

/**
 * A Wi-Fi network, held while sync runs. Wear OS turns Wi-Fi off while the phone is connected
 * over Bluetooth and routes the watch's traffic through the phone; asking for a network with
 * the Wi-Fi transport brings Wi-Fi up (if the watch knows the network), and binding the
 * process to it sends the core's sockets that way rather than through the phone, where the
 * other devices can't be reached. Released, Wi-Fi may go off again.
 */
private class WifiRequest(context: Context) {
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private var callback: ConnectivityManager.NetworkCallback? = null

    fun request() {
        if (callback != null) return
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                Log.i(TAG, "Wi-Fi is up: sync goes over it")
                connectivity.bindProcessToNetwork(network)
            }

            override fun onLost(network: Network) {
                Log.i(TAG, "Wi-Fi went away")
                connectivity.bindProcessToNetwork(null)
            }
        }
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        try {
            connectivity.requestNetwork(request, callback)
            this.callback = callback
        } catch (e: RuntimeException) {
            // SecurityException without CHANGE_NETWORK_STATE, or too many requests: sync still
            // works wherever the watch already is.
            Log.w(TAG, "Couldn't ask for Wi-Fi", e)
        }
    }

    fun release() {
        val callback = callback ?: return
        this.callback = null
        connectivity.bindProcessToNetwork(null)
        try {
            connectivity.unregisterNetworkCallback(callback)
        } catch (e: IllegalArgumentException) {
            // Already gone.
        }
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
