package ng.kithso.android.sync

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.os.BatteryManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.SystemClock
import android.util.Log
import android.widget.Toast
import androidx.compose.runtime.Immutable
import androidx.core.content.ContextCompat
import ng.kithso.core.CoreSync
import ng.kithso.core.HerePreview
import ng.kithso.core.HereResult
import ng.kithso.core.HereState
import ng.kithso.core.KithCoreException
import ng.kithso.core.NearbyDevice
import ng.kithso.core.ProfileItem
import ng.kithso.core.ProfileItemKind
import ng.kithso.android.R
import ng.kithso.core.SyncActivity
import ng.kithso.core.SyncArchiveOffer
import ng.kithso.core.SyncDevice
import ng.kithso.core.SyncIssueKind
import ng.kithso.core.SyncEstimate
import ng.kithso.core.SyncOutlook
import ng.kithso.core.SyncProblems
import ng.kithso.core.SyncProfile
import ng.kithso.core.SyncWaiting
import ng.kithso.core.TranscodeTarget
import ng.kithso.core.VerifyResult
import ng.kithso.core.excludeItem as coreExcludeItem
import ng.kithso.core.selectItems as coreSelectItems
import ng.kithso.core.hereState as coreHereState
import ng.kithso.core.herePreview as coreHerePreview
import ng.kithso.core.hereTakesAny as coreHereTakesAny
import ng.kithso.core.removeHere as coreRemoveHere
import ng.kithso.core.restoreHere as coreRestoreHere
import ng.kithso.core.BinEntry
import ng.kithso.core.binCount as coreBinCount
import ng.kithso.core.binOffered as coreBinOffered
import ng.kithso.core.binRestore as coreBinRestore
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.data.DeviceRemover
import ng.kithso.android.data.RecycleBin
import ng.kithso.android.data.ProfileChoices
import java.io.File
import java.net.Inet4Address
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Where a pairing has got to, as the pairing dialog shows it. */
@Immutable
sealed interface Pairing {
    /**
     * This phone asked [peer] to pair, and [peer] shows a code for the user to type in here.
     * [attemptsLeft] is set after a wrong code; [checking] while a code is being tried.
     */
    data class EnterCode(
        val peer: String,
        val attemptsLeft: Int? = null,
        val checking: Boolean = false,
    ) : Pairing

    /** [peer] asked to pair: its user types [code] in there. [attemptsLeft] drops after a wrong code. */
    data class ShowCode(
        val peer: String,
        val code: String,
        val attemptsLeft: Int,
    ) : Pairing

    /**
     * It's over: paired, or why not. [initiator]: this phone asked. When paired, [joined] says
     * this phone joined [peer]'s group (or a new one syncing [peer]'s library), so it's this
     * phone's sync settings that are to be chosen, else [peerUid]'s; [archiveName] is then the
     * group's archive (the first of [archives]), "" if not known. Not paired, [peerUid] is
     * [peer]'s if known ("" if not).
     */
    data class Ended(
        val peer: String,
        val reason: CoreSync.PairReason,
        val initiator: Boolean,
        val peerUid: String = "",
        val joined: Boolean = false,
        val archiveName: String = "",
        val archives: Int = 0,
        /** [CoreSync.PairReason.Version]: which device to update. */
        val versionText: String = "",
    ) : Pairing
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
    /**
     * The files the member is fetching from this phone: [sendsDone] (answered) of [sendsTotal],
     * as it announced them when its turn started; 0 before, and with a version too old to
     * announce them. Kept as the files are.
     */
    val sendsDone: Int = 0,
    val sendsTotal: Int = 0,
    /** File bytes both ways, for the whole session so far. */
    val bytesReceived: Long = 0,
    val bytesSent: Long = 0,
    /** Speed and time left, as the core words them; "" for neither. Only in [CoreSync.Phase.Files]. */
    val pace: String = "",
) {
    /** Files both ways, for a bar over the whole files phase: [moved] of [moving]. */
    val moving: Int get() = filesTotal + sendsTotal
    val moved: Int get() = filesDone.coerceAtMost(filesTotal) + sendsDone.coerceAtMost(sendsTotal)

    /** The member's turn: it's fetching from this phone, or that's all the session moves. */
    val sending: Boolean get() = sendsTotal > 0 && (sendsDone < sendsTotal || filesTotal == 0)
}

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
    /** What the next sync would copy, and whether this phone's user has chosen what it syncs; null outside a group. */
    val outlook: SyncOutlook? = null,
    /** What the last session that moved or deleted files did ("Last sync: ↑ 4 · ↓ 100"). */
    val activity: SyncActivity = SyncActivity.None,
    /** What each member syncs in one sentence ("3 albums and 2 playlists"), by uid. */
    val summaries: Map<String, String> = emptyMap(),
    /** Devices on the network that aren't in the group, by name. */
    val nearby: List<NearbyDevice> = emptyList(),
    /** The sessions running, by member uid. */
    val sessions: Map<String, SyncProgress> = emptyMap(),
    /** How the last session with each member since sync started ended, by member uid. */
    val results: Map<String, CoreSync.Event.SyncDone> = emptyMap(),
    val pairing: Pairing? = null,
    /** Tracks waiting for a member this phone can't reach now, for the main list's card. */
    val waiting: SyncWaiting = SyncWaiting.NONE,
    /** What's wrong with the files here, for the main list's other card. */
    val problems: SyncProblems = SyncProblems.NONE,
    /** Deletes go to the recycle bin here ([Syncer.binEntries]): in a group, not an archive. */
    val binOffered: Boolean = false,
    /** Deletes in the recycle bin: while any, this phone syncs only when Sync is pressed. */
    val binCount: Int = 0,
    /** Plugged in: this phone transcodes for other members whenever they ask. */
    val charging: Boolean = false,
    /** Transcoding for another member now, on the charger, while a sync screen is open, or in [background]. */
    val transcoding: Boolean = false,
    /**
     * Who it transcodes for while [transcoding], and how many tracks each is still to get, those
     * under way first; read every few seconds. Empty between the encodes of a session that asks
     * for one at a time, and while not transcoding.
     */
    val transcodingFor: List<TranscodeTarget> = emptyList(),
    /**
     * Background was pressed ([Syncer.keepInBackground]): the session and its transcoding go on
     * off screen, on battery too, through [SyncService], until they're over or cancelled.
     */
    val background: Boolean = false,
    /** The offer to make this phone the archive in place of a lost one; null if none. */
    val offer: SyncArchiveOffer? = null,
    /**
     * How the last session that ended went, when tracks are still missing ("Finished, but 3 tracks
     * are still missing"); "" otherwise, and once another starts.
     */
    val finished: String = "",
    /**
     * This phone's user cancelled sync, or another device's user cancelled a session with it,
     * and nothing has synced since: the pill says so until Sync is pressed or a session starts.
     */
    val cancelled: Boolean = false,
) {
    val inGroup: Boolean get() = libraryState != CoreSync.LibraryState.NoGroup

    /** In a group, this phone fetches nothing until its user chooses what it holds and presses Sync. */
    val needsChoice: Boolean get() = inGroup && outlook?.chosen == false

    /**
     * The red dot on the main menu and its Sync: deletes wait in the recycle bin while a member is online, so
     * a sync could run but won't until the user presses Sync.
     */
    val binDot: Boolean get() = binCount > 0 && members.any { it.online }

    val syncing: Set<String> get() = sessions.keys

    /**
     * Something is under way that shouldn't be cut off: a session or a pairing, or work kept
     * going in the [background].
     */
    val busy: Boolean get() = syncing.isNotEmpty() || pairing is Pairing.EnterCode || pairing is Pairing.ShowCode ||
        background

    /**
     * Transcoding on battery, which lasts only while a sync screen is open unless [background]:
     * the banner says to stay, plug in, or press Background.
     */
    val transcodingOnBattery: Boolean get() = transcoding && !charging
}

/**
 * LAN sync with the other devices in this phone's group, through the core ([CoreSync]). It
 * belongs to the process, not the UI. The core listens, discovers and syncs on its own threads
 * while sync runs, and sync runs while something [hold]s it: the app on screen, and
 * [SyncService] while a session or pairing is under way. A phone app in the background may be
 * frozen at any moment, so it doesn't listen all the time.
 *
 * Sync starts sessions by itself only while the app is on screen, as the core schedules them.
 * Once the app leaves the screen, [SyncService] only lets what's under way finish: the core
 * starts nothing new by itself ([CoreSync.setAutomatic]), though it still serves a session
 * another device starts meanwhile, and sync stops as soon as it's idle.
 *
 * Synced files land in [source]'s library folder, in the app's own storage (the core writes
 * nowhere else, so never on an SD card). The library is read again after every session that
 * brought or removed something, and now and then while files arrive. What files this phone
 * holds follows its profile ([profile], [editProfile]), which any member may edit.
 *
 * This phone starts out as [deviceName]. Renamed here ([rename]) or by another member, it takes
 * on the new name and passes it to [onRenamed], to be saved and given as [deviceName] next time.
 *
 * Members that want Opus get it from a member that transcodes for them. This phone does while
 * it's plugged in, and on battery while a sync screen is open and the app is on screen
 * ([setSyncScreenShown]) or once the user pressed Background ([keepInBackground]); see
 * [mayTranscode]. Otherwise leaving withdraws it at once, and the members count what's left as
 * missing until next time. Plugged in, a session under way as the app leaves goes on through
 * [SyncService], transcoding included, until it's over.
 *
 * Every call on the core runs on one thread ([thread]); the core's events arrive on its threads
 * and are queued over to that one.
 *
 * It's open only so the store screenshots (src/test/.../screenshots) can show a sync part way
 * through, through [status], [profile], [estimate] and [profileChoices], without the core.
 */
open class Syncer(
    private val context: Context,
    private val source: CoreLibrarySource,
    private val configDir: File,
    deviceName: String,
    private val scope: CoroutineScope,
    private val onRenamed: (String) -> Unit = {},
) : DeviceRemover, RecycleBin {
    private val thread = Dispatchers.IO.limitedParallelism(1)
    private val state = MutableStateFlow(SyncStatus(deviceName))
    open val status: StateFlow<SyncStatus> = state
    private val nothingToDoFlow = MutableSharedFlow<Unit>(extraBufferCapacity = 1)

    /**
     * A press of Sync now with every member found nothing to do: each member it asked answered,
     * and none of the sessions failed or did anything (files, rows, covers). The pill shakes.
     */
    open val nothingToDo: SharedFlow<Unit> = nothingToDoFlow

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

    // When the library was last read again for files that arrived (elapsed realtime). Only
    // touched on [thread].
    private var lastFilesRefresh = 0L

    // Members that refused a press of Sync as busy, tried once more since. Only touched on [thread].
    private val busyRetried = HashSet<String>()

    // The press of Sync now with every member waiting to be answered: the members it asked that
    // haven't, and whether any session so far failed or changed something. Only touched on [thread].
    private var pressWaiting = emptySet<String>()
    private var pressDidSomething = false

    // Reads who this phone transcodes for, every few seconds while it does. Only touched on [thread].
    private var transcodingWatch: Job? = null

    // When the app last went to the background (elapsed realtime), 0 for not yet. Only
    // touched on [thread].
    private var backgroundedAt = 0L

    // A sync screen is open and resumed. Only touched on [thread].
    private var syncScreenShown = false

    // The lost archives the problems last named, for which [SyncStatus.offer] was read; null
    // to read it again. Only touched on [thread].
    private var offerFor: Set<String>? = null

    // Tells the core when the network changes while sync runs. Only touched on [thread].
    private val networkWatch = NetworkWatch(context, scope, thread, { context.lanAddress() }) {
        val sync = sync?.takeIf { state.value.running } ?: return@NetworkWatch
        Log.i(TAG, "Network changed")
        recheckNetwork(sync)
    }

    // On [thread]: discovery asks again and reach checks run now, so which members are online
    // is current, after a network change or back from the background (where Android may have
    // frozen the process and its sockets with it, and missed members coming and going).
    private fun recheckNetwork(sync: CoreSync) {
        // Android may drop the lock with the network it was taken on.
        multicastLock?.release()
        multicastLock?.acquire()
        sync.networkChanged()
        refreshLists(sync)
    }

    init {
        scope.launch(thread) {
            for (event in events) onEvent(event)
        }
        watchPower()
    }

    // On [thread]: [SyncStatus.transcodingFor] kept current while [active].
    private fun watchTranscoding(active: Boolean) {
        transcodingWatch?.cancel()
        transcodingWatch = null
        if (!active) return
        transcodingWatch = scope.launch(thread) {
            while (true) {
                val sync = sync?.takeIf { state.value.running && state.value.transcoding } ?: break
                val targets = sync.transcodingFor()
                if (targets != state.value.transcodingFor) update { copy(transcodingFor = targets) }
                delay(TRANSCODING_POLL_MS)
            }
        }
    }

    /**
     * A sync screen is open with the app resumed ([shown]), or no longer is. On battery this
     * phone transcodes for other members only meanwhile.
     */
    fun setSyncScreenShown(shown: Boolean) {
        scope.launch(thread) {
            if (shown == syncScreenShown) return@launch
            syncScreenShown = shown
            reportPower()
        }
    }

    // Plugged in or not, from the sticky battery broadcast, now and on each change. The
    // receiver lives as long as the process, as this does.
    private fun watchPower() {
        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_BATTERY_CHANGED)
            addAction(Intent.ACTION_POWER_CONNECTED)
            addAction(Intent.ACTION_POWER_DISCONNECTED)
        }
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                // Connected and disconnected carry no extras; the sticky one is current.
                val battery = if (intent.action == Intent.ACTION_BATTERY_CHANGED) intent
                else context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
                val charging = when (intent.action) {
                    Intent.ACTION_POWER_CONNECTED -> true
                    Intent.ACTION_POWER_DISCONNECTED -> false
                    else -> battery.isPluggedIn()
                }
                scope.launch(thread) {
                    if (charging == state.value.charging) return@launch
                    update { copy(charging = charging) }
                    reportPower()
                }
            }
        }
        val sticky = ContextCompat.registerReceiver(
            context.applicationContext, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        val charging = sticky.isPluggedIn()
        scope.launch(thread) { if (charging != state.value.charging) update { copy(charging = charging) } }
    }

    // On [thread]. What the core publishes to the members, and acts on at once.
    private fun reportPower() {
        val sync = sync ?: return
        val charging = state.value.charging
        sync.setPower(onPower = charging, mayTranscode = mayTranscode(charging, syncScreenShown, holders))
    }

    /**
     * The Background button on the transcoding notice: the session under way, and this phone's
     * transcoding for it, go on after the app leaves the screen, on battery too, kept up by
     * [SyncService] and its notification, until no session runs and nothing is being
     * transcoded, Cancel sync ([endBackground]), or Android stops the service. Only while the
     * app is on screen, as Android lets the service start only then; nothing happens if no
     * work is under way.
     */
    fun keepInBackground() {
        scope.launch(thread) {
            val status = state.value
            if (HOLDER_APP !in holders || !status.running) return@launch
            if (status.sessions.isEmpty() && !status.transcoding) return@launch
            Log.i(TAG, "Sync goes on in the background")
            holders += HOLDER_BACKGROUND
            update { copy(background = true) }
            reportPower()
            SyncService.start(context)
        }
    }

    /**
     * Ends what [keepInBackground] began, as Cancel sync in its notification or Android's time
     * limit does. On battery, transcoding then stops at once unless a sync screen is open.
     */
    fun endBackground() {
        scope.launch(thread) { dropBackground() }
    }

    // On [thread].
    private fun dropBackground() {
        if (HOLDER_BACKGROUND !in holders) return
        Log.i(TAG, "Sync no longer goes on in the background")
        holders -= HOLDER_BACKGROUND
        update { copy(background = false) }
        reportPower()
        stopIfIdle()
    }

    // On [thread]. The work [keepInBackground] kept going is over once no session runs and no
    // encode is queued or running.
    private fun endBackgroundIfDone() {
        val status = state.value
        if (status.sessions.isEmpty() && !status.transcoding) dropBackground()
    }

    /**
     * Keeps sync running until [release] by the same [holder]. Only [HOLDER_APP], the app on
     * screen, lets it start sessions by itself.
     */
    fun hold(holder: String) {
        scope.launch(thread) {
            holders += holder
            val sync = sync
            if (!state.value.running) {
                start()
            } else if (sync != null) {
                if (holder == HOLDER_APP) {
                    sync.setAutomatic(true)
                    reportPower()
                    // Back after a while: what discovery saw before may be out of date.
                    val away = SystemClock.elapsedRealtime() - backgroundedAt
                    if (backgroundedAt > 0 && away >= RECHECK_AFTER_MS) recheckNetwork(sync) else refreshLists(sync)
                } else {
                    refreshLists(sync)
                }
            }
        }
    }

    fun release(holder: String) {
        scope.launch(thread) {
            holders -= holder
            // Off the screen: what's under way may finish, but nothing new starts, and on
            // battery transcoding for others stops at once.
            if (holder == HOLDER_APP) {
                backgroundedAt = SystemClock.elapsedRealtime()
                sync?.setAutomatic(false)
                reportPower()
            }
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

    /** Gives up the pairing in progress, either way round; the dialog closes. */
    fun cancelPairing() = call { sync ->
        val pairing = state.value.pairing
        sync.pairCancel()
        if (pairing?.let { it !is Pairing.Ended } == true) update { copy(pairing = null) }
    }

    /** Forgets how the last pairing ended, once it has been seen. */
    fun dismissPairing() = call {
        if (state.value.pairing is Pairing.Ended) update { copy(pairing = null) }
    }

    /**
     * The Sync button: syncs with [uid] (every member for null) now. It also chooses this
     * phone's profile as it stands, if it wasn't chosen, so its files sync from then on.
     */
    fun syncNow(uid: String? = null) = call { sync ->
        if (uid == null) busyRetried.clear() else busyRetried -= uid
        update { copy(cancelled = false) }
        sync.syncNow(uid)
        if (uid == null) {
            pressWaiting = state.value.members.filter { it.online }.map { it.uid }.toSet()
            pressDidSomething = false
        }
        refreshLists(sync)
    }

    /**
     * The Cancel button: ends the session with [uid] (every member's for null), deleting a file
     * it had half fetched. None starts with it for an hour, unless [syncNow] says so. The member
     * is told, and starts none with us by itself for 10 minutes; its sessions end as cancelled
     * (peerCancelled), not failed.
     */
    fun cancelSync(uid: String? = null) = call {
        it.cancel(uid)
        pressWaiting = emptySet()
        if (uid == null) update { copy(cancelled = true) }
    }

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
            } catch (e: KithCoreException) {
                Log.w(TAG, "Couldn't rename this phone", e)
                return@launch
            }
            sync.notifyChanged()
            refreshLists(sync)
        }
    }

    /**
     * The library changed here, so the other devices should hear of it soon; a delete may
     * have gone to the recycle bin, which holds syncing back.
     */
    fun notifyChanged() = call { sync ->
        sync.notifyChanged()
        refreshLists(sync)
    }

    // Profiles. These work whenever sync has been opened, running or not (the app holds it
    // while on screen), and throw KithCoreException: ERR_NOT_FOUND if it hasn't been or this
    // phone isn't in a group, or [uid] isn't a member; see CoreSync for the others.

    /** [uid]'s profile, this phone's ([SyncStatus.self]) or any other member's. */
    open suspend fun profile(uid: String): SyncProfile = onCore { it.profile(uid) }

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
        // The core passes it on by itself too; this makes sure it's soon.
        sync.notifyChanged()
        // A member's name is its profile's.
        refreshLists(sync)
        sync.profile(uid)
    }

    /**
     * Select all ([on]) or Select none: [uids], the items of [kind] the editor shows, after
     * [edit], what's waiting to be stored. Returns the profile as it now reads.
     */
    suspend fun selectItems(
        uid: String,
        edit: (SyncProfile) -> SyncProfile,
        kind: ProfileItemKind,
        uids: List<String>,
        on: Boolean,
    ): SyncProfile = onCore { sync ->
        val stored = sync.profile(uid)
        val edited = edit(stored)
        if (edited != stored) sync.setProfile(uid, edited)
        sync.coreSelectItems(uid, kind, uids, on)
        sync.notifyChanged()
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
            } catch (e: KithCoreException) {
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
     * A tick under Sync everything on [uid]'s profile: with [on], excludes [item], else brings
     * it back. Returns the profile as it now reads.
     */
    suspend fun excludeItem(uid: String, item: ProfileItem, on: Boolean): SyncProfile = onCore { sync ->
        sync.coreExcludeItem(uid, item.kind, item.uid, on)
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
    open suspend fun estimate(uid: String, profile: SyncProfile): SyncEstimate = onCore { it.estimate(uid, profile) }

    // Remove from this phone ([DeviceRemover]). Like the profile calls, these work whenever
    // sync has been opened; failures are logged and reported as null or false.

    override suspend fun hereState(kind: ProfileItemKind, uid: String): HereState = try {
        onCore { it.coreHereState(kind, uid) }
    } catch (e: KithCoreException) {
        HereState.Unavailable
    }

    override suspend fun herePreview(kind: ProfileItemKind, uid: String): HerePreview? = try {
        onCore { it.coreHerePreview(kind, uid) }
    } catch (e: KithCoreException) {
        Log.w(TAG, "Couldn't work out removing $uid", e)
        null
    }

    override suspend fun takesAny(kind: ProfileItemKind, uid: String, trackUids: List<String>): Boolean = try {
        onCore { it.coreHereTakesAny(kind, uid, trackUids) }
    } catch (e: KithCoreException) {
        Log.w(TAG, "Couldn't work out removing $uid", e)
        false
    }

    override suspend fun removeHere(kind: ProfileItemKind, uid: String): HereResult? = try {
        onCore { it.coreRemoveHere(kind, uid) }.also { source.refresh() }
    } catch (e: KithCoreException) {
        Log.w(TAG, "Couldn't remove $uid from this phone", e)
        null
    }

    override suspend fun restoreHere(kind: ProfileItemKind, uid: String): Boolean = try {
        onCore { it.coreRestoreHere(kind, uid) }
        // Bring it back soon, rather than at the next scheduled sync; but not while deletes
        // wait in the recycle bin, which a sync would make for good.
        if (state.value.binCount == 0) syncNow()
        true
    } catch (e: KithCoreException) {
        Log.w(TAG, "Couldn't sync $uid to this phone again", e)
        false
    }

    // The recycle bin ([RecycleBin]): the core keeps it in the library, and takes an entry back
    // through sync, as it becomes a removal from this phone.

    /** Whether deletes go to the recycle bin now; false if sync hasn't been opened. */
    suspend fun binOffered(): Boolean = try {
        onCore { it.coreBinOffered }
    } catch (e: KithCoreException) {
        false
    }

    override suspend fun binEntries(): List<BinEntry> = source.binEntries()

    override suspend fun binRestore(entry: BinEntry): Int? = try {
        onCore { sync -> sync.coreBinRestore(entry.id).also { refreshLists(sync) } }.also {
            Log.i(TAG, "Restored ${entry.title} from the recycle bin ($it entries)")
            source.refresh()
        }
    } catch (e: KithCoreException) {
        Log.w(TAG, "Couldn't restore ${entry.title} from the recycle bin", e)
        // A sync may have emptied it meanwhile.
        withContext(thread) { sync?.let { refreshLists(it) } }
        null
    }

    /** What a profile can select, from the whole library. Throws KithCoreException. */
    open suspend fun profileChoices(): ProfileChoices = source.profileChoices()

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
     * Goes on with the weekly deep check for up to [budgetMs], and returns true once it's over
     * for this week. Off [thread], as the core holds nothing up while it reads, so call it only
     * between [hold] and [release]. Throws KithCoreException.
     */
    suspend fun verifyWeekly(budgetMs: Long): Boolean {
        val sync = onCore { it }
        return withContext(Dispatchers.IO) { sync.verifyWeekly(budgetMs) }
    }

    /**
     * Removes those of [uids] that no device has any more from the library, on every device,
     * for good. The core works out once more which those are, so what came back stays. None of
     * them has a file here, so nothing that's playing goes with them.
     */
    suspend fun removeLost(uids: List<String>): LostRemoval {
        val removed = try {
            onCore { sync -> sync.removeLost(uids) }
        } catch (e: KithCoreException) {
            Log.w(TAG, "Couldn't remove the lost tracks", e)
            return if (e.status == CoreSync.ERR_LOCKED) LostRemoval.Busy else LostRemoval.Failed
        }
        Log.i(TAG, "Removed $removed of ${uids.size} lost tracks")
        if (removed > 0) source.tracksRemoved()
        return if (removed < uids.size) LostRemoval.Kept(uids.size - removed) else LostRemoval.Removed
    }

    /** Forgets the notices among the problems, once the user has seen them. */
    fun dismissNotices() = call { it.dismissNotices() }

    /** The one-time dialog for the lost archive [uid] was shown: it stays a notice from now on. */
    fun archiveOfferSeen(uid: String) = call { sync ->
        sync.archiveOfferSeen(uid)
        offerFor = null
        refreshLists(sync)
    }

    /**
     * Starts making this phone the archive in place of the lost [uid]. False if the core refused:
     * it's no longer on offer, there isn't room, or a takeover already runs.
     */
    suspend fun takeOverArchive(uid: String): Boolean = try {
        onCore { sync ->
            sync.takeOverArchive(uid)
            offerFor = null
            refreshLists(sync)
        }
        Log.i(TAG, "Taking over from the archive $uid")
        true
    } catch (e: KithCoreException) {
        Log.w(TAG, "Couldn't take over from the archive $uid", e)
        false
    }

    private fun startPairing(peer: String, request: (CoreSync) -> Unit) = call { sync ->
        try {
            request(sync)
            update { copy(pairing = Pairing.EnterCode(peer)) }
        } catch (e: KithCoreException) {
            // Not found: it left the network. Locked: a pairing is already under way.
            if (e.status != CoreSync.ERR_LOCKED) {
                update { copy(pairing = Pairing.Ended(peer, CoreSync.PairReason.Failed, initiator = true)) }
            }
            throw e
        }
        startService()
    }

    // On [thread]: keeps the process going until a session or pairing is over, should the app
    // leave the screen meanwhile. Only started while the app is on screen: from the background
    // Android won't start it, and nothing starts there anyway.
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

    // Runs [block] on [thread] with sync as last opened, running or not, and returns its result.
    private suspend fun <T> onCore(block: (CoreSync) -> T): T = withContext(thread) {
        block(sync ?: throw KithCoreException(CoreSync.ERR_NOT_FOUND))
    }

    private inline fun update(change: SyncStatus.() -> SyncStatus) {
        state.value = state.value.change()
    }

    // On [thread].
    private suspend fun start() {
        val sync = sync ?: try {
            configDir.mkdirs()
            source.openSync(configDir.path, state.value.deviceName) { events.trySend(it) }
        } catch (e: KithCoreException) {
            Log.e(TAG, "Couldn't open sync", e)
            null
        }.also { sync = it }
        if (sync == null) {
            update { copy(error = context.getString(R.string.sync_error_open)) }
            return
        }
        multicastLock?.acquire()
        // Started for the service alone (the app left the screen just before): no new sessions.
        sync.setAutomatic(HOLDER_APP in holders)
        reportPower()
        try {
            sync.start()
        } catch (e: KithCoreException) {
            Log.e(TAG, "Couldn't start sync", e)
            multicastLock?.release()
            update { copy(error = context.getString(R.string.sync_error_listen)) }
            return
        }
        Log.i(TAG, "Sync running on port ${sync.port}")
        networkWatch.start()
        update { copy(running = true, error = null) }
        refreshLists(sync)
    }

    // On [thread]. Stops once nothing holds sync. A session or pairing under way holds it
    // through [SyncService]; without that the process may be frozen or cut off from the
    // network at any moment, so it's ended rather than left hanging.
    private fun stopIfIdle() {
        val sync = sync ?: return
        if (holders.isNotEmpty() || !state.value.running) return
        networkWatch.stop()
        sync.stop()
        multicastLock?.release()
        update {
            copy(
                running = false, sessions = emptyMap(), nearby = emptyList(), port = 0, transcoding = false,
                transcodingFor = emptyList(), background = false,
            )
        }
        Log.i(TAG, "Sync stopped")
    }

    // On [thread].
    private fun onEvent(event: CoreSync.Event) {
        val sync = sync ?: return
        val pairing = state.value.pairing
        when (event) {
            is CoreSync.Event.Peer -> Unit
            // Read by refreshLists below, with the problems; a version, with the devices.
            CoreSync.Event.Waiting -> Unit
            is CoreSync.Event.Transcoding -> {
                update { copy(transcoding = event.active, transcodingFor = if (event.active) transcodingFor else emptyList()) }
                watchTranscoding(event.active)
                endBackgroundIfDone()
                return
            }
            // refreshLists below shows the device in no group.
            is CoreSync.Event.GroupDissolved -> {
                val text = context.getString(R.string.sync_group_dissolved, event.deviceName)
                scope.launch(Dispatchers.Main) { Toast.makeText(context, text, Toast.LENGTH_LONG).show() }
            }
            is CoreSync.Event.Version -> Unit
            CoreSync.Event.Outlook -> Unit
            is CoreSync.Event.PairCode -> {
                update { copy(pairing = Pairing.ShowCode(event.requester, event.code, event.attemptsLeft)) }
                startService()
            }
            is CoreSync.Event.PairResult -> if (!onPairResult(event, pairing)) return
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
                // Refused for its version: the device's row and page say which to update, without
                // a failure each time it tries again.
                if (!event.user && event.status == CoreSync.ERR_VERSION) return
                // A press refused as busy, most often just after Cancel while the other device is
                // still ending the session cancelled: tried once more after a moment rather than
                // dropped, so Sync now does something.
                if (event.user && event.status == CoreSync.ERR_LOCKED && busyRetried.add(event.uid)) {
                    Log.i(TAG, "${event.uid} was busy; trying again")
                    update { copy(sessions = sessions - event.uid) }
                    scope.launch(thread) {
                        delay(BUSY_RETRY_MS)
                        sync.takeIf { state.value.running }?.syncNow(event.uid)
                    }
                    return
                }
                if (event.status != CoreSync.ERR_LOCKED) busyRetried -= event.uid
                if (event.user && event.uid in pressWaiting) {
                    pressWaiting = pressWaiting - event.uid
                    pressDidSomething = pressDidSomething || event.status != 0 || event.didSomething
                    if (pressWaiting.isEmpty() && !pressDidSomething) nothingToDoFlow.tryEmit(Unit)
                }
                update {
                    copy(
                        sessions = sessions - event.uid,
                        results = results + (event.uid to event),
                        finished = if (event.librarySynced) event.finishedText else finished,
                        cancelled = cancelled || event.peerCancelled,
                    )
                }
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
        endBackgroundIfDone()
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
        val progress = SyncProgress(
            phase = event.phase,
            rowsSent = event.rowsSent,
            rowsReceived = event.rowsReceived,
            filesDone = if (files) event.filesDone else before?.filesDone ?: 0,
            filesTotal = if (files) event.filesTotal else before?.filesTotal ?: 0,
            sendsDone = if (files) event.sendsDone else before?.sendsDone ?: 0,
            sendsTotal = if (files) event.sendsTotal else before?.sendsTotal ?: 0,
            bytesReceived = if (files) event.bytesReceived else before?.bytesReceived ?: 0,
            bytesSent = if (files) event.bytesSent else before?.bytesSent ?: 0,
            pace = if (files) event.pace else "",
        )
        update { copy(sessions = sessions + (event.uid to progress)) }
        // Show what has arrived so far on a long transfer, rather than only at the end.
        val now = SystemClock.elapsedRealtime()
        if (files && progress.filesDone > (before?.filesDone ?: 0) && now - lastFilesRefresh >= FILES_REFRESH_MS) {
            lastFilesRefresh = now
            source.refresh()
        }
        if (before != null) return false
        update { copy(finished = "", cancelled = false) }
        startService()
        return true
    }

    // On [thread]. False if the result needs no telling, so nothing changed.
    private fun onPairResult(event: CoreSync.Event.PairResult, pairing: Pairing?): Boolean {
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
                Pairing.Ended(peer, event.reason, event.initiator, event.uid, event.joined, event.archiveName, event.archives)
            }
            CoreSync.PairReason.Version ->
                Pairing.Ended(peer, event.reason, event.initiator, event.uid, versionText = event.versionText)
            else -> Pairing.Ended(peer, event.reason, event.initiator, event.uid)
        }
        Log.i(TAG, "Pairing with $peer: ${event.reason}, status ${event.status}")
        update { copy(pairing = next) }
        return true
    }

    // On [thread]. Reads the group and what's nearby again.
    private fun refreshLists(sync: CoreSync) {
        try {
            val devices = sync.devices()
            val libraryState = sync.libraryState
            val waiting = sync.waiting()
            val problems = sync.problems()
            val offer = readOffer(sync, problems.issues.filter { it.kind in OFFER_KINDS }.map { it.deviceUid }.toSet())
            val binOffered = sync.coreBinOffered
            val activity = sync.lastActivity()
            val summaries = devices.filter { !it.isSelf && !it.removed }.associate { device ->
                device.uid to try {
                    sync.profileSummary(device.uid)
                } catch (e: KithCoreException) {
                    ""
                }
            }
            val binCount = sync.coreBinCount
            // Renamed here, or through a desktop's older editor in a session.
            val name = sync.deviceName
            if (name.isNotEmpty() && name != state.value.deviceName) onRenamed(name)
            update {
                copy(
                    deviceName = name.ifEmpty { deviceName },
                    libraryState = libraryState,
                    self = devices.firstOrNull { it.isSelf },
                    members = devices.filter { !it.isSelf && !it.removed },
                    outlook = if (libraryState != CoreSync.LibraryState.NoGroup) sync.outlook() else null,
                    activity = activity,
                    summaries = summaries,
                    waiting = waiting,
                    problems = problems,
                    offer = offer,
                    binOffered = binOffered,
                    binCount = binCount,
                    nearby = if (running) sync.nearby() else emptyList(),
                    onLan = context.isOnLan(),
                    address = context.lanAddress(),
                    port = if (running) sync.port else 0,
                )
            }
        } catch (e: KithCoreException) {
            Log.e(TAG, "Couldn't read the sync group", e)
        }
    }

    // On [thread]. The offer to take over from a lost archive, read again only when the lost
    // archives the problems name change ([lost]), as reading it reads every track.
    private fun readOffer(sync: CoreSync, lost: Set<String>): SyncArchiveOffer? {
        if (lost == offerFor) return state.value.offer
        offerFor = lost
        if (lost.isEmpty()) return null
        return try {
            sync.archiveOffer()
        } catch (e: KithCoreException) {
            Log.w(TAG, "Couldn't read the archive offer", e)
            null
        }
    }

    companion object {
        // The issues that come with an offer to take over from a lost archive.
        private val OFFER_KINDS = setOf(SyncIssueKind.ArchiveLost, SyncIssueKind.ArchiveReinstall)

        /** What the app holds sync as while it's on screen. */
        const val HOLDER_APP = "app"
        const val HOLDER_VERIFY = "verify"

        /** What [keepInBackground] holds sync as, until the work under way is over. */
        const val HOLDER_BACKGROUND = "background"

        /**
         * Whether this phone may transcode for other members: while it's [charging], while the
         * app is on screen ([HOLDER_APP]) with a sync screen open ([syncScreenShown]), and
         * after Background was pressed ([HOLDER_BACKGROUND]) until that work is over.
         */
        fun mayTranscode(charging: Boolean, syncScreenShown: Boolean, holders: Set<String>): Boolean =
            charging || HOLDER_BACKGROUND in holders || (syncScreenShown && HOLDER_APP in holders)

        private const val TAG = "kithsong"
        private const val FILES_REFRESH_MS = 10_000L
        private const val BUSY_RETRY_MS = 2_000L
        // Away at least this long, the network is checked again on coming back (see [hold]).
        private const val RECHECK_AFTER_MS = 10_000L
        private const val TRANSCODING_POLL_MS = 2_000L
    }
}

/** How [Syncer.removeLost] went. */
sealed interface LostRemoval {
    data object Removed : LostRemoval

    /** [tracks] stayed: they turned up again, or their files couldn't be removed. */
    data class Kept(val tracks: Int) : LostRemoval

    /** A session is moving files, one of them perhaps: nothing went. */
    data object Busy : LostRemoval
    data object Failed : LostRemoval
}

/** Whether a battery broadcast says the phone is on a charger; false for none. */
private fun Intent?.isPluggedIn(): Boolean = (this?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0

/** On Wi-Fi or Ethernet, where sync can find the other devices. */
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
