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

/**
 * Pairing needs the two libraries merged (CoreSync.Event.PairMerge): the larger is kept. If
 * [keptHere], this phone's is, and the other device's user is asked; else this phone's
 * library is set aside and its music merged into the other's, once its user agrees
 * ([answered] after Merge). [tracksHere] and [tracksThere] are each library's track count;
 * [scrobbleUser] is this phone's Last.fm user, to be signed out ("" if none is), and
 * [forgetNames] the paired devices this phone forgets.
 */
@Immutable
data class MergeProposal(
    val keptHere: Boolean,
    val tracksHere: Long,
    val tracksThere: Long,
    val scrobbleUser: String = "",
    val forgetNames: List<String> = emptyList(),
    val answered: Boolean = false,
) {
    /** This phone's user is still to say Merge or Cancel. */
    val needsAnswer: Boolean get() = !keptHere && !answered
}

/** Where a pairing has got to, as the pairing dialog shows it. */
@Immutable
sealed interface Pairing {
    /** The merge pairing needs, if it does (shown with the code, or in its place until answered). */
    val merge: MergeProposal?

    /**
     * This phone asked [peer] to pair, and [peer] shows a code for the user to type in here.
     * [attemptsLeft] is set after a wrong code; [checking] while a code is being tried.
     */
    data class EnterCode(
        val peer: String,
        val attemptsLeft: Int? = null,
        val checking: Boolean = false,
        override val merge: MergeProposal? = null,
    ) : Pairing

    /** [peer] asked to pair: its user types [code] in there. [attemptsLeft] drops after a wrong code. */
    data class ShowCode(
        val peer: String,
        val code: String,
        val attemptsLeft: Int,
        override val merge: MergeProposal? = null,
    ) : Pairing

    /**
     * It's over: paired, or why not. [initiator]: this phone asked. When paired, [joined] says
     * this phone joined [peer]'s group (or a new one syncing [peer]'s library), so it's this
     * phone's sync settings that are to be chosen, else [peerUid]'s; [archiveName] is then the
     * group's archive (the first of [archives]), "" if not known. [merged]: paired by merging
     * libraries, this phone's kept if [keptHere]; [scrobbleSignedOut] is the Last.fm user
     * signed out on the merged device, "" if none was.
     */
    data class Ended(
        val peer: String,
        val reason: CoreSync.PairReason,
        val initiator: Boolean,
        val peerUid: String = "",
        val joined: Boolean = false,
        val archiveName: String = "",
        val archives: Int = 0,
        val merged: Boolean = false,
        val keptHere: Boolean = false,
        val scrobbleSignedOut: String = "",
    ) : Pairing {
        override val merge: MergeProposal? get() = null
    }
}

/**
 * The merge of this phone's set-aside library into the one it now syncs, after pairing by
 * merging: [done] of [total] in [phase] while it runs, then its [result] until dismissed.
 */
@Immutable
data class MergeState(
    val phase: CoreSync.MergePhase = CoreSync.MergePhase.Tracks,
    val done: Int = 0,
    val total: Int = 0,
    val result: CoreSync.Event.MergeDone? = null,
) {
    val running: Boolean get() = result == null

    /** How far it has got, 0 to 1: albums take most of the time, so nine tenths go to them. */
    val fraction: Float get() {
        val part = if (total > 0) done.coerceIn(0, total).toFloat() / total else 0f
        return if (phase == CoreSync.MergePhase.Tracks) part * 0.9f else 0.9f + part * 0.1f
    }
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
    /** The merge after pairing by merging libraries, while it runs and until its summary is dismissed. */
    val merge: MergeState? = null,
    /** This phone's old library is still to be merged in, after the next sync with the group. */
    val mergePending: Boolean = false,
) {
    val inGroup: Boolean get() = libraryState != CoreSync.LibraryState.NoGroup

    /** The members a session is running with. */
    val syncing: Set<String> get() = sessions.keys

    /** Something is under way that shouldn't be cut off: a session, a pairing or a merge. */
    val busy: Boolean get() =
        syncing.isNotEmpty() || pairing is Pairing.EnterCode || pairing is Pairing.ShowCode || merge?.running == true
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

    // Merging libraries, only touched on [thread]. The merge proposal comes just before the
    // code on the phone showing it, so it waits here for the code. Once this phone's user
    // declined a merge, the pairing's end needs no telling (on the phone showing the code it
    // comes only when the other device's user enters it).
    private var pendingMerge: MergeProposal? = null
    private var declined = false
    private var debugMergePending = false

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

    /**
     * Gives up the pairing in progress, either way round. While a merge waits for this phone's
     * answer, it declines the merge instead, which ends the pairing (on the phone showing the
     * code, once the other device's user enters it); the dialog closes either way.
     */
    fun cancelPairing() = call { sync ->
        val pairing = state.value.pairing
        if (pairing?.merge?.needsAnswer == true) {
            declined = true
            try {
                sync.pairMergeConfirm(false)
            } catch (e: MusicCoreException) {
                sync.pairCancel()
            }
        } else {
            sync.pairCancel()
        }
        if (pairing?.let { it !is Pairing.Ended } == true) update { copy(pairing = null) }
    }

    /** Merge pressed: this phone's library is merged into the other device's once they pair. */
    fun confirmMerge() = call { sync ->
        val pairing = state.value.pairing
        if (pairing?.merge?.needsAnswer != true) return@call
        // Not found: the pairing is over, and its result follows.
        sync.pairMergeConfirm(true)
        val merge = pairing.merge?.copy(answered = true)
        update {
            copy(
                pairing = when (pairing) {
                    is Pairing.EnterCode -> pairing.copy(merge = merge)
                    is Pairing.ShowCode -> pairing.copy(merge = merge)
                    is Pairing.Ended -> pairing
                },
            )
        }
    }

    /** Forgets the merge's summary, once it has been seen. */
    fun dismissMerge() {
        scope.launch(thread) {
            if (state.value.merge?.running == false) update { copy(merge = null) }
        }
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

    /**
     * Debug builds only (MainActivity's fake_merge extra): shows one of the merge's screens
     * with made-up data, for screenshots without a second device. The buttons on them call
     * the core as usual, which has no such pairing.
     */
    fun debugShowMerge(scenario: String) {
        scope.launch(thread) {
            val peer = "Living room PC"
            val ask = MergeProposal(false, 120, 1204, "tania_listens", listOf("Laptop", "Kitchen tablet"))
            val kept = MergeProposal(true, 1204, 120)
            val done = CoreSync.Event.MergeDone(
                0, "/data/user/0/ng.kithso/files/library/library-before-merge-2026-10-29.db",
                80, 6, 4, 30, 5, 2, 1, 6, 1, 2, 1, 40, 10, "tania_listens", 3, 2,
            )
            val ended = Pairing.Ended(peer, CoreSync.PairReason.Paired, initiator = true, peerUid = "fake", joined = true)
            when (scenario) {
                "enter_ask" -> update { copy(pairing = Pairing.EnterCode(peer, merge = ask)) }
                "enter_confirmed" -> update { copy(pairing = Pairing.EnterCode(peer, merge = ask.copy(answered = true))) }
                "enter_kept" -> update { copy(pairing = Pairing.EnterCode(peer, merge = kept)) }
                "show_ask" -> update { copy(pairing = Pairing.ShowCode(peer, "123456", 3, ask)) }
                "show_confirmed" -> update { copy(pairing = Pairing.ShowCode(peer, "123456", 3, ask.copy(answered = true))) }
                "show_kept" -> update { copy(pairing = Pairing.ShowCode(peer, "123456", 3, kept)) }
                "paired_merged" -> update {
                    copy(pairing = ended.copy(merged = true, scrobbleSignedOut = "tania_listens"))
                }
                "paired_kept" -> update {
                    copy(pairing = ended.copy(joined = false, merged = true, keptHere = true, scrobbleSignedOut = "tania_listens"))
                }
                "declined" -> update { copy(pairing = Pairing.Ended(peer, CoreSync.PairReason.MergeDeclined, true)) }
                "mismatch" -> update { copy(pairing = Pairing.Ended(peer, CoreSync.PairReason.LibraryMismatch, true)) }
                "progress" -> update { copy(merge = MergeState(CoreSync.MergePhase.Tracks, 12, 40)) }
                "done" -> update { copy(merge = MergeState(result = done)) }
                "failed" -> update { copy(merge = MergeState(result = done.copy(status = -4))) }
                "pending" -> {
                    debugMergePending = true
                    update { copy(mergePending = true) }
                }
            }
        }
    }

    private fun startPairing(peer: String, request: (CoreSync) -> Unit) = call { sync ->
        declined = false
        pendingMerge = null
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
                // A merge declined here: the dialog stays closed, even for a new code.
                if (declined) return
                val merge = (pairing as? Pairing.ShowCode)?.merge ?: pendingMerge
                pendingMerge = null
                update { copy(pairing = Pairing.ShowCode(event.requester, event.code, event.attemptsLeft, merge)) }
                startService()
            }
            is CoreSync.Event.PairResult -> if (!onPairResult(event, pairing)) return
            is CoreSync.Event.PairMerge -> {
                onPairMerge(event, pairing)
                return
            }
            is CoreSync.Event.MergeProgress -> {
                onMergeProgress(event)
                return
            }
            is CoreSync.Event.MergeDone -> {
                Log.i(
                    TAG,
                    "Merged the old library: status ${event.status}, ${event.tracksAdded} added, " +
                        "${event.tracksBetter} better, ${event.tracksAttached + event.tracksMapped} matched, " +
                        "${event.tracksSkipped} deleted, ${event.tracksLeftOut} left out, ${event.tracksFailed} failed",
                )
                update { copy(merge = (merge ?: MergeState()).copy(result = event)) }
                source.refresh()
                lastFilesRefresh = SystemClock.elapsedRealtime()
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

    // On [thread]. The proposal goes with the code: on the initiator it comes while the code
    // is being entered (or once it has been); on the phone showing the code, just before it.
    private fun onPairMerge(event: CoreSync.Event.PairMerge, pairing: Pairing?) {
        val proposal = MergeProposal(
            keptHere = event.keptHere,
            tracksHere = event.tracksHere,
            tracksThere = event.tracksThere,
            scrobbleUser = if (event.scrobbleSignOut) event.scrobbleUser else "",
            forgetNames = event.forgetNames,
        )
        Log.i(TAG, "Pairing with ${event.name} merges libraries; kept here: ${event.keptHere}")
        when {
            event.initiator -> if (pairing is Pairing.EnterCode) update { copy(pairing = pairing.copy(merge = proposal)) }
            pairing is Pairing.ShowCode -> update { copy(pairing = pairing.copy(merge = proposal)) }
            else -> {
                // A new incoming pairing: one declined before is over.
                declined = false
                pendingMerge = proposal
            }
        }
    }

    // On [thread].
    private fun onMergeProgress(event: CoreSync.Event.MergeProgress) {
        val before = state.value.merge
        update { copy(merge = MergeState(event.phase, event.done, event.total)) }
        // Albums come into the library as it goes.
        val now = SystemClock.elapsedRealtime()
        if (now - lastFilesRefresh >= FILES_REFRESH_MS) {
            lastFilesRefresh = now
            source.refresh()
        }
        if (before?.running != true) startService()
    }

    // On [thread]. False if the result needs no telling, so nothing changed.
    private fun onPairResult(event: CoreSync.Event.PairResult, pairing: Pairing?): Boolean {
        if (event.reason != CoreSync.PairReason.WrongCode) pendingMerge = null
        if (declined) {
            // This phone's user declined the merge: the end it brings (MergeDeclined, or
            // Cancelled) needs no telling, nor do wrong codes typed on the other device.
            Log.i(TAG, "Pairing after declining the merge: ${event.reason}")
            if (event.reason == CoreSync.PairReason.WrongCode) return false
            declined = false
            if (event.reason != CoreSync.PairReason.Paired) {
                update { copy(pairing = null) }
                return true
            }
        }
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
                Pairing.Ended(
                    peer, event.reason, event.initiator, event.uid, event.joined, event.archiveName, event.archives,
                    event.merged, event.keptHere, event.scrobbleSignedOut,
                )
            }
            else -> Pairing.Ended(peer, event.reason, event.initiator)
        }
        Log.i(TAG, "Pairing with $peer: ${event.reason}, status ${event.status}, merged ${event.merged}")
        // Wrong codes keep the merge's proposal or note with the code.
        val kept = if (next is Pairing.EnterCode) next.copy(merge = pairing?.merge) else next
        update { copy(pairing = kept) }
        return true
    }

    // On [thread]. Reads the group and what's nearby again.
    private fun refreshLists(sync: CoreSync) {
        try {
            val devices = sync.devices()
            val libraryState = sync.libraryState
            val mergePending = sync.mergePending
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
                    mergePending = mergePending || debugMergePending,
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
