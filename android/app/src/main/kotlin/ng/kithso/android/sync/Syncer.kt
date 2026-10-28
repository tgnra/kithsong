package ng.kithso.android.sync

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.Immutable
import ng.kithso.core.ConfirmState
import ng.kithso.core.CoreSync
import ng.kithso.core.HerePreview
import ng.kithso.core.HereResult
import ng.kithso.core.HereState
import ng.kithso.core.MusicCoreException
import ng.kithso.core.NearbyDevice
import ng.kithso.core.ProfileItemKind
import ng.kithso.android.R
import ng.kithso.core.SyncDevice
import ng.kithso.core.SyncEstimate
import ng.kithso.core.SyncProfile
import ng.kithso.core.VerifyResult
import ng.kithso.core.hereState as coreHereState
import ng.kithso.core.herePreview as coreHerePreview
import ng.kithso.core.removeHere as coreRemoveHere
import ng.kithso.core.restoreHere as coreRestoreHere
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.data.DeviceRemover
import ng.kithso.android.data.ProfileChoices
import java.io.File
import java.net.Inet4Address
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

/** Where a pairing has got to, as the pairing dialog shows it. */
@Immutable
sealed interface Pairing {
    /**
     * This phone asked [peer] to pair, and [peer] shows a code for the user to type in here.
     * [attemptsLeft] is set after a wrong code; [checking] while a code is being tried.
     */
    data class EnterCode(val peer: String, val attemptsLeft: Int? = null, val checking: Boolean = false) : Pairing

    /** [peer] asked to pair: its user types [code] in there. [attemptsLeft] drops after a wrong code. */
    data class ShowCode(val peer: String, val code: String, val attemptsLeft: Int) : Pairing

    /** It's over: paired, or why not. [initiator]: this phone asked. */
    data class Ended(val peer: String, val reason: CoreSync.PairReason, val initiator: Boolean) : Pairing
}

/** How far a session with a member has got (mc_sync_progress). */
@Immutable
data class SyncProgress(
    val phase: CoreSync.Phase,
    /** Change rows, for the whole session so far. */
    val rowsSent: Long = 0,
    val rowsReceived: Long = 0,
    /**
     * The files this phone is fetching in this session: [filesDone] (received, failed or
     * refused) of [filesTotal]. Set from [CoreSync.Phase.Files] on, and kept from then.
     */
    val filesDone: Int = 0,
    val filesTotal: Int = 0,
    /** File bytes both ways, for the whole session so far. */
    val bytesReceived: Long = 0,
    val bytesSent: Long = 0,
)

/** What the devices section of settings and the pairing dialog show. */
@Immutable
data class SyncStatus(
    /** What other devices call this phone; see [Syncer.rename]. */
    val deviceName: String,
    /** Listening, discovering and syncing; only while something holds it (see [Syncer.hold]). */
    val running: Boolean = false,
    /** Why sync couldn't start, if it couldn't. */
    val error: String? = null,
    /** On Wi-Fi or Ethernet, where the other devices are. */
    val onLan: Boolean = true,
    /** Where other devices can reach this phone, e.g. for adding it by address; null if unknown. */
    val address: String? = null,
    val port: Int = 0,
    val libraryState: CoreSync.LibraryState = CoreSync.LibraryState.NoGroup,
    /** This phone as a member of the group, e.g. for its uid and file status; null outside one. */
    val self: SyncDevice? = null,
    /** The other members of the group, removed ones left out, by name. */
    val members: List<SyncDevice> = emptyList(),
    /** Whether this phone's own files sync on their own; null outside a group. */
    val selfConfirm: ConfirmState? = null,
    /** Devices on the network that aren't in the group, by name. */
    val nearby: List<NearbyDevice> = emptyList(),
    /** The sessions running, by member uid. */
    val sessions: Map<String, SyncProgress> = emptyMap(),
    /** How the last session with each member since sync started ended, by member uid. */
    val results: Map<String, CoreSync.Event.SyncDone> = emptyMap(),
    val pairing: Pairing? = null,
) {
    val inGroup: Boolean get() = libraryState != CoreSync.LibraryState.NoGroup

    /** The members a session is running with. */
    val syncing: Set<String> get() = sessions.keys

    /** Something is under way that shouldn't be cut off: a session or a pairing. */
    val busy: Boolean get() = syncing.isNotEmpty() || pairing is Pairing.EnterCode || pairing is Pairing.ShowCode
}

/**
 * LAN sync with the other devices in this phone's group, through the core ([CoreSync]). It
 * belongs to the process, not the UI. The core listens, discovers and syncs on its own threads
 * while sync runs, and sync runs while something [hold]s it: the app on screen, [SyncService]
 * while a session or pairing is under way, and [SyncWorker] now and then in the background. A
 * phone app in the background may be frozen at any moment, so it doesn't listen all the time.
 *
 * Synced data and audio files land in [source]'s library folder, in the app's own storage (the
 * core's file phase writes into the library folder, never anywhere else, so never on an SD
 * card). The library is read again after every session that brought or removed something, and
 * now and then while files arrive. This phone serves its own files to the others too; the core
 * does that on its own. What files it holds follows its profile ([profile], [editProfile]),
 * which any member may edit.
 *
 * This phone starts out as [deviceName]. Renamed here ([rename]) or by another member, it takes
 * on the new name and passes it to [onRenamed], to be saved and given as [deviceName] next time.
 *
 * Every call on the core runs on one thread ([thread]); the core's events arrive on its threads
 * and are queued over to that one.
 */
class Syncer(
    private val context: Context,
    private val source: CoreLibrarySource,
    private val configDir: File,
    deviceName: String,
    private val scope: CoroutineScope,
    private val onRenamed: (String) -> Unit = {},
) : DeviceRemover {
    private val thread = Dispatchers.IO.limitedParallelism(1)
    private val state = MutableStateFlow(SyncStatus(deviceName))
    val status: StateFlow<SyncStatus> = state

    // Only touched on [thread].
    private var sync: CoreSync? = null
    private val holders = HashSet<String>()
    private val multicastLock: WifiManager.MulticastLock? by lazy {
        // Without it Android drops incoming multicast, and discovery (mDNS) hears nothing.
        context.applicationContext.getSystemService(WifiManager::class.java)
            ?.createMulticastLock("kithsong:sync")
            ?.apply { setReferenceCounted(false) }
    }
    private val events = Channel<CoreSync.Event>(Channel.UNLIMITED)
    private var scheduled: Boolean? = null

    // Sessions that have ended, for [syncOnce] to wait on. Only touched on [thread].
    private val done = MutableStateFlow(emptySet<String>())

    // When the library was last read again for files that arrived (elapsed realtime). Only
    // touched on [thread].
    private var lastFilesRefresh = 0L

    init {
        scope.launch(thread) {
            for (event in events) onEvent(event)
        }
    }

    /** Keeps sync running until [release] by the same [holder]. */
    fun hold(holder: String) {
        scope.launch(thread) {
            holders += holder
            val sync = sync
            if (!state.value.running) start() else if (sync != null) refreshLists(sync)
        }
    }

    fun release(holder: String) {
        scope.launch(thread) {
            holders -= holder
            stopIfIdle()
        }
    }

    /** Asks [device], seen on the network, to pair. Its code is then typed in with [enterCode]. */
    fun pair(device: NearbyDevice) = startPairing(device.name) { it.pairRequest(device.uid) }

    /** Asks the device at [host] to pair, for networks where discovery doesn't reach it. */
    fun pair(host: String, port: Int) = startPairing(host) { it.pairRequestAddress(host, port) }

    fun enterCode(code: String) = call { sync ->
        val pairing = state.value.pairing as? Pairing.EnterCode ?: return@call
        update { copy(pairing = pairing.copy(checking = true)) }
        sync.pairEnter(code)
    }

    /** Gives up the pairing in progress, either way round. */
    fun cancelPairing() = call { sync ->
        sync.pairCancel()
        if (state.value.pairing?.let { it !is Pairing.Ended } == true) update { copy(pairing = null) }
    }

    /** Forgets how the last pairing ended, once it has been seen. */
    fun dismissPairing() = call {
        if (state.value.pairing is Pairing.Ended) update { copy(pairing = null) }
    }

    /**
     * The Sync button: syncs with [uid] (every member for null) now, confirming this phone's
     * profile and its, so their files sync from then on. Background work uses [syncOnce].
     */
    fun syncNow(uid: String? = null) = call { sync ->
        sync.confirmSyncNow(uid)
        refreshLists(sync)
    }

    /**
     * The Cancel button: ends the session with [uid] (every member's for null), deleting a file
     * it had half fetched. None starts with it for an hour, unless [syncNow] says so.
     */
    fun cancelSync(uid: String? = null) = call { it.cancel(uid) }

    fun removeDevice(uid: String) = call { sync ->
        sync.removeDevice(uid)
        refreshLists(sync)
    }

    /** Leaves the group. The library stays as it is. */
    fun leaveGroup() = call { sync ->
        sync.leaveGroup()
        refreshLists(sync)
    }

    /** Renames this phone, for the whole group when in one. Works whether sync runs or not. */
    fun rename(name: String) {
        scope.launch(thread) {
            val sync = sync ?: return@launch
            try {
                sync.deviceName = name
            } catch (e: MusicCoreException) {
                Log.w(TAG, "Couldn't rename this phone", e)
                return@launch
            }
            sync.notifyChanged()
            refreshLists(sync)
        }
    }

    /** The library changed here, so the other devices should hear of it soon. */
    fun notifyChanged() = call { it.notifyChanged() }

    // Profiles. These work whenever sync has been opened, running or not (the app holds it
    // while on screen), and throw MusicCoreException: ERR_NOT_FOUND if it hasn't been or this
    // phone isn't in a group, or [uid] isn't a member; see CoreSync for the others.

    /** [uid]'s profile, this phone's ([SyncStatus.self]) or any other member's. */
    suspend fun profile(uid: String): SyncProfile = onCore { it.profile(uid) }

    /**
     * Changes [uid]'s profile with [edit], applied to the one stored now and stored straight
     * away, so edits made meanwhile on other devices to other fields or items aren't
     * overwritten. Returns what's stored afterwards, which may include such edits. The name of
     * this phone's own profile is what the group calls it, so leave it be and [rename] instead.
     * ERR_LOCKED for an archive's mode, format or budget switch.
     */
    suspend fun editProfile(uid: String, edit: (SyncProfile) -> SyncProfile): SyncProfile = onCore { sync ->
        val stored = sync.profile(uid)
        val edited = edit(stored)
        if (edited == stored) return@onCore stored
        sync.setProfile(uid, edited)
        // The core passes it on by itself too; this just makes sure it's soon.
        sync.notifyChanged()
        // A member's name is its profile's.
        refreshLists(sync)
        sync.profile(uid)
    }

    /**
     * [editProfile] for when nobody waits on the result, such as edits left over as the editor
     * closes; failures are only logged.
     */
    fun editProfileLater(uid: String, edit: (SyncProfile) -> SyncProfile) {
        scope.launch {
            try {
                editProfile(uid, edit)
            } catch (e: MusicCoreException) {
                Log.w(TAG, "Couldn't store $uid's profile", e)
            }
        }
    }

    /**
     * Makes [uid] an archive, keeping the whole library as originals, or stops it being one.
     * Any member may make any device one; only this phone may stop being one itself, and only
     * once another archive has verified it holds everything (ERR_LOCKED otherwise). Returns the
     * profile as it now reads.
     */
    suspend fun setArchive(uid: String, archive: Boolean): SyncProfile = onCore { sync ->
        sync.setArchive(uid, archive)
        sync.notifyChanged()
        refreshLists(sync)
        sync.profile(uid)
    }

    /**
     * How much [uid] would hold with [profile], stored or not (a profile editor's candidate):
     * tracks, size, the bitrate a budget picks, or how far over budget it is. Reads every
     * track, so it may take a moment on a big library. ERR_LIBRARY_MISMATCH if this phone's
     * library isn't the group's.
     */
    suspend fun estimate(uid: String, profile: SyncProfile): SyncEstimate = onCore { it.estimate(uid, profile) }

    // Remove from this phone ([DeviceRemover]). Like the profile calls, these work whenever
    // sync has been opened; failures are logged and reported as null or false.

    override suspend fun hereState(kind: ProfileItemKind, uid: String): HereState = try {
        onCore { it.coreHereState(kind, uid) }
    } catch (e: MusicCoreException) {
        HereState.Unavailable
    }

    override suspend fun herePreview(kind: ProfileItemKind, uid: String): HerePreview? = try {
        onCore { it.coreHerePreview(kind, uid) }
    } catch (e: MusicCoreException) {
        Log.w(TAG, "Couldn't work out removing $uid", e)
        null
    }

    override suspend fun removeHere(kind: ProfileItemKind, uid: String): HereResult? = try {
        onCore { it.coreRemoveHere(kind, uid) }.also { source.refresh() }
    } catch (e: MusicCoreException) {
        Log.w(TAG, "Couldn't remove $uid from this phone", e)
        null
    }

    override suspend fun restoreHere(kind: ProfileItemKind, uid: String): Boolean = try {
        onCore { it.coreRestoreHere(kind, uid) }
        syncNow() // bring it back soon, rather than at the next scheduled sync
        true
    } catch (e: MusicCoreException) {
        Log.w(TAG, "Couldn't sync $uid to this phone again", e)
        false
    }

    /** What a profile can select, from the whole library. Throws MusicCoreException. */
    suspend fun profileChoices(): ProfileChoices = source.profileChoices()

    /**
     * Checks this phone's files, [deep]ly by reading every one, else by size and time, and
     * reports them to the group. Failed copies are deleted, to be fetched again. Waits for a
     * file phase under way to end first, and holds up every other sync call until it's done,
     * so a deep check is for when nothing else is going on.
     */
    suspend fun verify(deep: Boolean): VerifyResult = onCore { sync ->
        sync.verify(deep).also { refreshLists(sync) }
    }

    /**
     * Syncs with every member once, for [SyncWorker]: runs sync if it isn't running, and waits
     * until each member has had a session or [timeoutMs] has passed. False if the app can't
     * reach the network now, so it should try again later. This is automatic sync, so the
     * core's hourly limit applies: a member synced completely within the last hour gets no
     * session until [SyncDevice.autoSyncAfterMs]. It is waited for only if that comes early
     * in the run (as it does when the last run synced it), and otherwise left to a later run.
     */
    suspend fun syncOnce(timeoutMs: Long): Boolean {
        // In the background Android may block the app's network, even while WorkManager runs
        // the work in-process, and the core's connections then hang rather than fail.
        if (!context.isOnLan()) return false
        hold(HOLDER_ONCE)
        try {
            val ready = withTimeoutOrNull(START_TIMEOUT_MS) { state.first { it.running || it.error != null } }
            if (ready?.running != true || !ready.inGroup) return true
            val members = withContext(thread) {
                val sync = sync ?: return@withContext emptySet<String>()
                done.value = emptySet()
                sync.syncBackground(null)
                // Read after the call, so the limit is as the core has it now.
                val due = System.currentTimeMillis() + timeoutMs / 2
                sync.devices().filter { !it.isSelf && !it.removed && it.autoSyncAfterMs < due }
                    .mapTo(HashSet()) { it.uid }
            }
            if (members.isNotEmpty()) withTimeoutOrNull(timeoutMs) { done.first { it.containsAll(members) } }
        } catch (e: MusicCoreException) {
            Log.w(TAG, "Couldn't sync", e)
        } finally {
            release(HOLDER_ONCE)
        }
        return true
    }

    private fun startPairing(peer: String, request: (CoreSync) -> Unit) = call { sync ->
        try {
            request(sync)
            update { copy(pairing = Pairing.EnterCode(peer)) }
        } catch (e: MusicCoreException) {
            // Not found: it left the network. Locked: a pairing is already under way.
            if (e.status != CoreSync.ERR_LOCKED) {
                update { copy(pairing = Pairing.Ended(peer, CoreSync.PairReason.Failed, initiator = true)) }
            }
            throw e
        }
        startService()
    }

    // On [thread]: keeps the process going until a session or pairing is over. Only while the
    // app is on screen: from the background Android won't start it, and there [SyncWorker]'s
    // job keeps the process going instead.
    private fun startService() {
        if (HOLDER_APP in holders) SyncService.start(context)
    }

    // Runs [block] on [thread] with the running sync, logging the core's failures.
    private fun call(block: (CoreSync) -> Unit) {
        scope.launch(thread) {
            val sync = sync?.takeIf { state.value.running } ?: return@launch
            try {
                block(sync)
            } catch (e: MusicCoreException) {
                Log.w(TAG, "Sync call failed", e)
            }
        }
    }

    // Runs [block] on [thread] with sync as last opened, running or not, and returns its result.
    private suspend fun <T> onCore(block: (CoreSync) -> T): T = withContext(thread) {
        block(sync ?: throw MusicCoreException(CoreSync.ERR_NOT_FOUND))
    }

    private inline fun update(change: SyncStatus.() -> SyncStatus) {
        state.value = state.value.change()
    }

    // On [thread].
    private suspend fun start() {
        val sync = sync ?: try {
            configDir.mkdirs()
            source.openSync(configDir.path, state.value.deviceName) { events.trySend(it) }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't open sync", e)
            null
        }.also { sync = it }
        if (sync == null) {
            update { copy(error = context.getString(R.string.sync_error_open)) }
            return
        }
        multicastLock?.acquire()
        try {
            sync.start()
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't start sync", e)
            multicastLock?.release()
            update { copy(error = context.getString(R.string.sync_error_listen)) }
            return
        }
        Log.i(TAG, "Sync running on port ${sync.port}")
        update { copy(running = true, error = null) }
        refreshLists(sync)
    }

    // On [thread]. Stops once nothing holds sync. A session or pairing under way holds it
    // through [SyncService]; without that the process may be frozen or cut off from the
    // network at any moment, so it's ended rather than left hanging.
    private fun stopIfIdle() {
        val sync = sync ?: return
        if (holders.isNotEmpty() || !state.value.running) return
        sync.stop()
        multicastLock?.release()
        update { copy(running = false, sessions = emptyMap(), nearby = emptyList(), port = 0) }
        Log.i(TAG, "Sync stopped")
    }

    // On [thread].
    private fun onEvent(event: CoreSync.Event) {
        val sync = sync ?: return
        val pairing = state.value.pairing
        when (event) {
            is CoreSync.Event.Peer -> Unit
            is CoreSync.Event.PairCode -> {
                update { copy(pairing = Pairing.ShowCode(event.requester, event.code, event.attemptsLeft)) }
                startService()
            }
            is CoreSync.Event.PairResult -> onPairResult(event, pairing)
            is CoreSync.Event.Progress -> if (!onProgress(event)) return
            is CoreSync.Event.SyncDone -> {
                Log.i(
                    TAG,
                    "Synced with ${event.uid}: status ${event.status}, ${event.rowsReceived} rows in, " +
                        "${event.rowsSent} out, ${event.filesReceived} files in (${event.bytesReceived} bytes), " +
                        "${event.filesSent} out, ${event.filesDeleted} deleted, ${event.filesMissing} missing, " +
                        "${event.filesCorrupt} corrupt, ${event.coversReceived} covers in",
                )
                update { copy(sessions = sessions - event.uid, results = results + (event.uid to event)) }
                done.value += event.uid
                // Albums, playlists and plays from the other devices, and the files and covers
                // that arrived or went: the library and Recents. Files that arrived before a
                // session failed are kept, so they count either way.
                val rows = event.status == 0 && event.rowsReceived > 0
                val files = event.filesReceived > 0 || event.filesDeleted > 0 || event.filesUnknown > 0
                if (rows || files || event.coversReceived > 0) {
                    source.refresh()
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
            // Never got past connecting: a scheduled attempt that found it unreachable, which
            // no SyncDone reports.
            if (before == null || before.phase == CoreSync.Phase.Connecting) done.value += event.uid
            return true
        }
        val files = event.phase == CoreSync.Phase.Files
        val progress = SyncProgress(
            phase = event.phase,
            rowsSent = event.rowsSent,
            rowsReceived = event.rowsReceived,
            filesDone = if (files) event.filesDone else before?.filesDone ?: 0,
            filesTotal = if (files) event.filesTotal else before?.filesTotal ?: 0,
            bytesReceived = if (files) event.bytesReceived else before?.bytesReceived ?: 0,
            bytesSent = if (files) event.bytesSent else before?.bytesSent ?: 0,
        )
        update { copy(sessions = sessions + (event.uid to progress)) }
        // Show what has arrived so far on a long transfer, rather than only at the end.
        val now = SystemClock.elapsedRealtime()
        if (files && progress.filesDone > (before?.filesDone ?: 0) && now - lastFilesRefresh >= FILES_REFRESH_MS) {
            lastFilesRefresh = now
            source.refresh()
        }
        if (before != null) return false
        startService()
        return true
    }

    // On [thread].
    private fun onPairResult(event: CoreSync.Event.PairResult, pairing: Pairing?) {
        val peer = event.name.ifEmpty {
            when (pairing) {
                is Pairing.EnterCode -> pairing.peer
                is Pairing.ShowCode -> pairing.peer
                else -> context.getString(R.string.sync_other_device)
            }
        }
        val next = when (event.reason) {
            // The other device's user typed the wrong code: keep showing it (on_pair_code
            // brings a new one after too many tries).
            CoreSync.PairReason.WrongCode -> if (event.initiator) {
                Pairing.EnterCode(peer, attemptsLeft = event.attemptsLeft)
            } else {
                (pairing as? Pairing.ShowCode)?.copy(attemptsLeft = event.attemptsLeft)
            }
            // Given up here, or by the other device before a code was tried.
            CoreSync.PairReason.Cancelled -> null
            CoreSync.PairReason.Paired -> {
                source.refresh()
                Pairing.Ended(peer, event.reason, event.initiator)
            }
            else -> Pairing.Ended(peer, event.reason, event.initiator)
        }
        Log.i(TAG, "Pairing with $peer: ${event.reason}, status ${event.status}")
        update { copy(pairing = next) }
    }

    // On [thread]. Reads the group and what's nearby again.
    private fun refreshLists(sync: CoreSync) {
        try {
            val devices = sync.devices()
            val libraryState = sync.libraryState
            // Renamed here, or by another member in a session.
            val name = sync.deviceName
            if (name.isNotEmpty() && name != state.value.deviceName) onRenamed(name)
            update {
                copy(
                    deviceName = name.ifEmpty { deviceName },
                    libraryState = libraryState,
                    self = devices.firstOrNull { it.isSelf },
                    members = devices.filter { !it.isSelf && !it.removed },
                    selfConfirm = devices.firstOrNull { it.isSelf }?.confirm,
                    nearby = if (running) sync.nearby() else emptyList(),
                    onLan = context.isOnLan(),
                    address = context.lanAddress(),
                    port = if (running) sync.port else 0,
                )
            }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't read the sync group", e)
        }
        val inGroup = state.value.inGroup
        if (scheduled != inGroup) {
            scheduled = inGroup
            SyncWorker.schedule(context, inGroup)
        }
    }

    companion object {
        /** What the app holds sync as while it's on screen. */
        const val HOLDER_APP = "app"

        private const val TAG = "kithsong"
        private const val HOLDER_ONCE = "once"
        private const val START_TIMEOUT_MS = 10_000L
        private const val FILES_REFRESH_MS = 10_000L
    }
}

/** Whether the phone is on Wi-Fi or Ethernet, where sync can find the other devices. */
fun Context.isOnLan(): Boolean {
    val connectivity = getSystemService(ConnectivityManager::class.java)
    val capabilities = connectivity.getNetworkCapabilities(connectivity.activeNetwork) ?: return false
    return capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ||
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
}

/** This phone's IPv4 address on the network it's on, or null. */
fun Context.lanAddress(): String? {
    val connectivity = getSystemService(ConnectivityManager::class.java)
    val link = connectivity.getLinkProperties(connectivity.activeNetwork) ?: return null
    return link.linkAddresses.map { it.address }.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }?.hostAddress
}
