package ng.kithso.core

/** mc_device_kind. */
enum class DeviceKind { Desktop, Phone }

/** A member of this device's sync group (mc_sync_device), this device included. */
data class SyncDevice(
    val uid: String,
    val name: String,
    private val kindCode: Int,
    val isSelf: Boolean,
    /** Seen by discovery, or synced within the last few minutes. */
    val online: Boolean,
    /** Keeps the whole library as originals. */
    val archive: Boolean,
    /** Removed from the group; still listed by the core. */
    val removed: Boolean,
    /** Where it was last seen or reached, or null if never. */
    val address: String?,
    val port: Int,
    /** The last session with it, in Unix ms; 0 if never. */
    val lastSyncMs: Long,
    /**
     * Unix ms before which this device starts no automatic session with it (an hour after the
     * last complete one, MC_SYNC_AUTO_INTERVAL_MS, or 10 minutes after its user cancelled one,
     * MC_SYNC_PEER_CANCEL_HOLD_MS); 0 if it may now. See [CoreSync.syncBackground].
     */
    val autoSyncAfterMs: Long,
    /**
     * Its own report of its files (sync_status). The rest are 0 without one: it hasn't
     * finished a file phase yet.
     */
    val hasStatus: Boolean,
    /** When it last checked its files, in Unix ms. */
    val verifiedMs: Long,
    /** The files its profile wants, those it holds and checked, missing ones, and corrupt ones. */
    val expected: Long,
    val ok: Long,
    val missing: Long,
    val corrupt: Long,
    /** The size of every file it holds. */
    val bytes: Long,
    /**
     * Which device to update when its Kithsong can't sync with this one's (mc_sync_version_text()),
     * in the core's language; "" when it can. See "Versions" in sync.h.
     */
    val versionText: String = "",
    /**
     * What's wrong with it, for its row's subtext, in the core's language (mc_sync_device_warning()):
     * its version, a hold on its side ("Open Kithsong on Pixel and press Sync"), it can't be
     * reached, something else answers at its address, an archive away for long, low space. "" if
     * nothing.
     */
    val warning: String = "",
    /** A calm note on when this phone next syncs with it, or what holds it back here (mc_sync_device_note()); "" if not known. */
    val note: String = "",
    /** How this phone and it reach each other, in words (mc_sync_reach_text()); for this phone's own row, whether others reach it. */
    val reachText: String = "",
    private val holdCode: Int = 0,
    private val peerHoldCode: Int = 0,
    /** Until when [peerHold] lasts, in Unix ms; 0 for no end. */
    val peerHoldUntilMs: Long = 0,
    /** When this phone would next sync with it by itself, in Unix ms; 0 if not known. */
    val nextAutoMs: Long = 0,
    /** It may convert for others but doesn't: too little free space. */
    val lowSpace: Boolean = false,
    /** When it was last seen: synced, discovered or connected, in Unix ms; 0 if never. */
    val lastSeenMs: Long = 0,
) {
    val kind: DeviceKind get() = DeviceKind.entries.getOrElse(kindCode) { DeviceKind.Desktop }

    /** Why this phone starts no automatic session with it now. */
    val hold: SyncHold get() = SyncHold.of(holdCode)

    /** What it says holds sync back on its side. */
    val peerHold: SyncHold get() = SyncHold.of(peerHoldCode)
}

/** mc_sync_hold: why a device starts no automatic session with another now. */
enum class SyncHold {
    None,

    /** Synced lately. */
    Rate,

    /** Its user cancelled. */
    Cancelled,

    /** The other device's user cancelled. */
    PeerCancelled,

    /** The recycle bin holds deletes: only a press of Sync goes. */
    Bin,

    /** Automatic sync is off (a phone with Kithsong closed). */
    AutoOff,

    /** Its user hasn't chosen what it keeps. */
    NotChosen,

    /** Their versions can't sync. */
    Version,
    ;

    companion object {
        fun of(code: Int): SyncHold = entries.getOrElse(code) { None }
    }
}

/** mc_sync_issue_kind, in its order. */
enum class SyncIssueKind {
    FolderUnwritable,
    DiskFull,
    WriteFailed,
    FilesBusy,
    FailedFiles,
    UnknownFiles,
    NoTranscoder,
    TranscoderAway,
    Refused,
    DeviceLacks,
    DeviceLeft,
    NoHolder,

    /** On an archive: tracks gone for good, which [CoreSync.removeLost] removes. */
    Lost,
    Held,
    LowSpace,
    WrongDevice,
    ArchiveAway,
    ArchiveDamaged,
    ArchiveFull,

    /** An archive unseen for 30 days: [CoreSync.archiveOffer] offers to take over. */
    ArchiveLost,

    /** This phone looks like the archive reinstalled: [CoreSync.archiveOffer] offers to take over. */
    ArchiveReinstall,
    Takeover,
    NoticeArchive,
    NoticeBudget,

    /** One this version doesn't know. */
    Other,
}

/**
 * Something that keeps sync from doing what it should, or a notice of something it did by itself
 * (mc_sync_issue), in the core's language for an app that syncs only while it's open: [title],
 * what happened ([body]) and how to fix it ([fix], "" if nothing). [deviceUid] is the device it's
 * about, this phone's own for its own.
 */
data class SyncIssue(
    private val kindCode: Int,
    val deviceUid: String,
    val deviceName: String,
    val notice: Boolean,
    val tracks: Int,
    val count: Int,
    val title: String,
    val body: String,
    val fix: String,
) {
    val kind: SyncIssueKind
        get() = SyncIssueKind.entries.getOrNull(kindCode) ?: SyncIssueKind.Other
}

/**
 * An offer to make this device the archive in place of a lost one (mc_sync_archive_offer), with
 * its dialog's [title], [body] and [button] in the core's language, worded for a phone; [button]
 * is "" when this device can't take over ([canTake] false). [asked]: the one-time dialog was
 * shown ([CoreSync.archiveOfferSeen]). [nobodyCan]: no member has room, so the body advises
 * pairing one. [reinstall]: this device looks like the lost one reinstalled.
 */
data class SyncArchiveOffer(
    val uid: String,
    val name: String,
    val asked: Boolean,
    val canTake: Boolean,
    val nobodyCan: Boolean,
    val reinstall: Boolean,
    /** Tracks no device left has the original of. */
    val unrecoverable: Int,
    val title: String,
    val body: String,
    val button: String,
)

/**
 * What the next sync would copy, roughly (mc_sync_outlook): tracks this device would fetch,
 * and what other members lack that this one could send. [chosen]: this device's user has
 * chosen what it syncs; until then it fetches nothing. [text] is the core's summary, such as
 * "↑ 10 GB · ↓ 100 tracks" or "Up to date"; "" until chosen.
 */
data class SyncOutlook(
    val chosen: Boolean,
    val receiveTracks: Int,
    val receiveBytes: Long,
    val sendTracks: Int,
    val sendBytes: Long,
    val sendDevices: Int,
    val text: String,
) {
    val upToDate: Boolean get() = receiveTracks == 0 && sendTracks == 0
}

/**
 * The last session that moved or deleted files here (mc_sync_activity), kept across restarts:
 * when it ended ([whenMs], 0 for none yet), its counts, and [text], "Last sync: ↑ 4 · ↓ 100 ·
 * 10 deleted" ("" for none).
 */
data class SyncActivity(
    val whenMs: Long,
    val received: Int,
    val sent: Int,
    val deleted: Int,
    val text: String,
) {
    companion object {
        val None = SyncActivity(0, 0, 0, 0, "")
    }
}

/** Tracks this device is transcoding for the member [uid] ("" if not said): mc_sync_transcode_target. */
data class TranscodeTarget(val uid: String, val tracks: Int)

/** mc_profile_item_kind. */
enum class ProfileItemKind {
    Album,

    /** Every album and track credited to the artist. */
    Artist,

    /** A manual playlist. */
    Playlist,

    /** A manual album group. */
    AlbumGroup,
}

/** An album, artist, playlist or album group a profile selects, by its library uid. */
data class ProfileItem(val kind: ProfileItemKind, val uid: String)

/**
 * What a member of the group holds (mc_sync_profile): everything or a selection, as originals
 * or Opus, within a size budget. Edit a copy and store it with [CoreSync.setProfile].
 */
data class SyncProfile(
    /** Its friendly name; null shows the device's own. */
    val name: String?,
    /**
     * It keeps the whole library as originals, whatever the rest says; read-only here, set
     * with [CoreSync.setArchive]. An archive's profile can't change [syncAll], [opusKbps] or
     * [budgetEnabled] (ERR_LOCKED).
     */
    val archive: Boolean,
    /** Everything; else just [items]. */
    val syncAll: Boolean,
    /** 0 for originals, else Opus at this bitrate, one of [CoreSync.OPUS_BITRATES]; ignored with a budget. */
    val opusKbps: Int,
    /** The Auto format: with [budgetBytes], the best of originals and each bitrate that fits. */
    val budgetEnabled: Boolean,
    val budgetBytes: Long,
    /** The selection; kept while [syncAll] so it comes back. */
    val items: List<ProfileItem>,
    /**
     * The albums and artists removed from the device (an artist's albums among them), read-only:
     * Select all leaves them out. Ticking one syncs it there again.
     */
    val removed: List<ProfileItem> = emptyList(),
) {
    // For jni_bridge.c, which passes the items and removed as parallel kinds and uids.
    @Suppress("unused")
    private constructor(
        name: String?,
        archive: Boolean,
        syncAll: Boolean,
        opusKbps: Int,
        budgetEnabled: Boolean,
        budgetBytes: Long,
        kinds: IntArray,
        uids: Array<String>,
        removedKinds: IntArray,
        removedUids: Array<String>,
    ) : this(
        name,
        archive,
        syncAll,
        opusKbps,
        budgetEnabled,
        budgetBytes,
        items(kinds, uids),
        items(removedKinds, removedUids),
    )

    private companion object {
        // Kinds this version doesn't know are left out by the core already. A function of its
        // own, as two maps inlined into the constructor make a class Robolectric (the store
        // screenshots) refuses to load: their local variables clash.
        fun items(kinds: IntArray, uids: Array<String>): List<ProfileItem> =
            kinds.indices.map { ProfileItem(ProfileItemKind.entries[kinds[it]], uids[it]) }
    }
}

/** How much a device would hold with a profile (mc_sync_estimate). */
data class SyncEstimate(
    /** The tracks the profile wants. */
    val tracks: Int,
    /** Their size at [kbps]; when [overBudget], at 64 kbps. */
    val totalBytes: Long,
    /** What it would hold them as: 0 for originals, else the Opus bitrate; 0 when [overBudget]. */
    val kbps: Int,
    /** It's an archive: everything, originals, no budget. */
    val archive: Boolean,
    /** Even 64 kbps doesn't fit the budget: nothing would sync. */
    val overBudget: Boolean,
    /** When [overBudget]: how much too big at 64 kbps. */
    val shortfallBytes: Long,
    /** With a budget, what the tracks must fit in (the budget, or less free space); -1 without. */
    val limitBytes: Long,
    /** The device's free space, -1 if unknown. */
    val freeBytes: Long,
    /** Tracks imported on this device the profile leaves out; 0 for another device. */
    val importsDropped: Int,
    /**
     * The tracks' size as originals ([0]) and at each of [CoreSync.OPUS_BITRATES] ([1 + i]),
     * whatever the profile's format, so an editor can show every option.
     */
    val stepBytes: List<Long>,
)

/** What [CoreSync.verify] found (mc_verify_result). */
data class VerifyResult(
    val checked: Long,
    val ok: Long,
    /** Missing, changed or corrupt. */
    val failed: Long,
    /** Failed copies received from other devices, deleted so the next session fetches them again. */
    val deleted: Long,
)

/** A member holding tracks that wait for it (mc_sync_waiting_device). */
data class WaitingDevice(
    val uid: String,
    /** What the group calls it. */
    val name: String,
    private val kindCode: Int,
    /** Of the waiting tracks, those it holds. */
    val tracks: Int,
) {
    val kind: DeviceKind get() = DeviceKind.entries.getOrElse(kindCode) { DeviceKind.Desktop }
}

/** A track no device has any more (mc_sync_lost_track). [artist] is empty if it has no credit. */
data class LostTrack(val uid: String, val title: String, val artist: String)

/**
 * What's wrong with the files here (mc_sync_problems), for the card at the top of the library.
 * [text] is the card's sentences in the core's language, empty if there's nothing to say.
 * [failed] files failed a check, [unknown] are in the library folder and not in the library, and
 * [lost] are the tracks an archive can offer to remove ([CoreSync.removeLost]).
 */
data class SyncProblems(
    val text: String,
    val failed: Int,
    val unknown: Int,
    val lost: List<LostTrack>,
    /** Every issue, the ones above included: problems first, then notices. */
    val issues: List<SyncIssue> = emptyList(),
) {
    // For jni_bridge.c, which passes the tracks as parallel arrays.
    @Suppress("unused")
    private constructor(
        text: String,
        failed: Int,
        unknown: Int,
        uids: Array<String>,
        titles: Array<String>,
        artists: Array<String>,
        issues: Array<SyncIssue>,
    ) : this(text, failed, unknown, uids.indices.map { LostTrack(uids[it], titles[it], artists[it]) }, issues.asList())

    companion object {
        val NONE = SyncProblems("", 0, 0, emptyList())
    }
}

/**
 * Tracks this device's profile wants that only members it can't reach now hold
 * (mc_sync_waiting): "12 tracks are waiting for Tania's desktop to come online." [tracks]
 * counts each once; [devices] are the members holding them, most tracks first, and a track
 * several of them hold counts for each. "Can't reach" is how the last session with a member
 * went, not discovery, so it doesn't flicker; [CoreSync.Event.Waiting] says when it changed.
 */
data class SyncWaiting(val tracks: Int, val devices: List<WaitingDevice>) {
    // For jni_bridge.c, which passes the devices as parallel arrays.
    @Suppress("unused")
    private constructor(
        tracks: Int,
        uids: Array<String>,
        names: Array<String>,
        kinds: IntArray,
        counts: IntArray,
    ) : this(tracks, uids.indices.map { WaitingDevice(uids[it], names[it], kinds[it], counts[it]) })

    companion object {
        val NONE = SyncWaiting(0, emptyList())
    }
}

/** A device discovery sees that isn't in this device's group (mc_sync_nearby). */
data class NearbyDevice(
    val uid: String,
    val name: String,
    private val kindCode: Int,
    /** It must leave its own group before it can pair with this one. */
    val inOtherGroup: Boolean,
    val address: String?,
    val port: Int,
    /** As [SyncDevice.versionText]: pairing with it will be refused when it isn't "". */
    val versionText: String = "",
) {
    val kind: DeviceKind get() = DeviceKind.entries.getOrElse(kindCode) { DeviceKind.Desktop }
}

/**
 * This device's sync state and LAN sync (mc_sync), for the library [library], whose folder
 * it syncs through connections of its own. [configDir] holds sync.db: this device's keys and
 * its group. Nothing touches the network until [start].
 *
 * [listener] gets each [Event] on one of the core's threads, one at a time, possibly before the
 * constructor has returned, so it should hand them to another thread. Every other call should
 * come from one thread at a time, except [notifyChanged], which any thread may call.
 *
 * Two libraries that both have music are never mixed: pairing them is refused with
 * [PairReason.LibraryMismatch].
 */
class CoreSync(
    library: CoreLibrary,
    configDir: String,
    deviceName: String,
    kind: DeviceKind,
    private val listener: (Event) -> Unit,
) : AutoCloseable {
    /** mc_pair_reason. */
    enum class PairReason { Paired, WrongCode, InGroup, LibraryMismatch, AlreadyPaired, Busy, Cancelled, Failed, Version }

    /** mc_sync_phase. */
    enum class Phase { Connecting, Group, Library, Files, Done }

    /** mc_sync_lib_state: whether the open library is the one the group syncs. */
    enum class LibraryState { NoGroup, Group, Other, Unknown }

    sealed interface Event {
        /** A discovered or group device came online or went offline; [member]: it's in the group. */
        data class Peer(val uid: String, val online: Boolean, val member: Boolean) : Event

        /** [requester] asked to pair: show [code] for its user to type in. */
        data class PairCode(val code: String, val requester: String, val attemptsLeft: Int) : Event

        /**
         * How a pairing attempt ended. [initiator]: this device asked, and its user typed the
         * code. [uid] and [name] are "" if the other device was never reached. After
         * [PairReason.WrongCode] on the initiator, the pairing waits for another code.
         * When paired, [joined] says this device joined (the other's group, or a new group
         * syncing the other's library): it holds nothing yet, and its sync settings are the
         * ones to choose. Then [archiveName] is the group's archive (the first by name of
         * [archives]), "" and 0 if the other device didn't say. After [PairReason.Version],
         * [versionText] says which device to update.
         */
        data class PairResult(
            val uid: String,
            val name: String,
            val initiator: Boolean,
            val reason: PairReason,
            val attemptsLeft: Int,
            val status: Int,
            val joined: Boolean = false,
            val archiveName: String = "",
            val archives: Int = 0,
            val versionText: String = "",
        ) : Event

        /**
         * A session's progress. Rows count the whole session so far. In [Phase.Files],
         * [filesDone] of [filesTotal] are the files this device is fetching (received, failed
         * or refused), [sendsDone] of [sendsTotal] the files the peer is fetching from it (as
         * it announced them when its turn started; 0 before, and from a version too old to
         * announce them), and the bytes count the whole session's files both ways; they're 0
         * in the other phases.
         *
         * [user]: this device's user asked for the session (the Sync button, pairing, leaving
         * the group). [connected]: the member has answered; false only in [Phase.Connecting]
         * and in the [Phase.Done] ending an attempt that never got that far. Show a session
         * once it's [connected], or from the start if [user]: most attempts are automatic
         * ones at members that may well be away ("Showing sync" in sync.h).
         */
        data class Progress(
            val uid: String,
            val phase: Phase,
            val rowsSent: Long,
            val rowsReceived: Long,
            val filesDone: Int,
            val filesTotal: Int,
            val bytesReceived: Long,
            val bytesSent: Long,
            val sendsDone: Int,
            val sendsTotal: Int,
            /** How fast files are moving and the time left, as the core words them; "" for neither. */
            val pace: String = "",
            val user: Boolean = false,
            val connected: Boolean = true,
        ) : Event

        /**
         * A session ended; [status] is 0 on success. [librarySynced]: library data was
         * exchanged too, and then the files counts are set, this device as the receiver:
         * fetched and verified, wanted but still without a file, failed their checksum twice,
         * deleted (no longer wanted, or failed the check after the session), and unknown to
         * the library (deleted, except on an archive). [filesSent] were served to the peer.
         * [coversReceived]: album covers fetched for folders here that had none. [cancelled]:
         * [CoreSync.cancel] here cut it short ([status] isn't 0 then). [peerCancelled]: the other
         * device's user cancelled it there, which is no failure to show ([status] isn't 0
         * either). [user] and [connected] as in [Progress]: a failure with neither is an
         * automatic attempt that got no proper answer, which needs no telling.
         */
        data class SyncDone(
            val uid: String,
            val status: Int,
            val librarySynced: Boolean,
            val converged: Boolean,
            val rowsSent: Long,
            val rowsReceived: Long,
            val filesReceived: Int,
            val filesMissing: Int,
            val filesCorrupt: Int,
            val filesDeleted: Int,
            val filesUnknown: Int,
            val filesSent: Int,
            val bytesReceived: Long,
            val bytesSent: Long,
            val coversReceived: Int = 0,
            val cancelled: Boolean = false,
            val peerCancelled: Boolean = false,
            val user: Boolean = false,
            val connected: Boolean = true,
            /** "Finished, but 3 tracks are still missing" in the core's language (mc_sync_finished_text()); "" when none are. */
            val finishedText: String = "",
        ) : Event {
            /** It moved or deleted files (mc_sync_result_changed()): the last activity is now its. */
            val changed: Boolean get() = filesReceived > 0 || filesSent > 0 || filesDeleted > 0

            /**
             * It did anything at all (mc_sync_result_did_something()): files, rows either way
             * (another device's edits, playlists, plays) or covers.
             */
            val didSomething: Boolean get() = changed || rowsSent > 0 || rowsReceived > 0 || coversReceived > 0
        }

        /** The tracks waiting for a device, or the problems, changed: read [CoreSync.waiting] and [CoreSync.problems] again. */
        data object Waiting : Event

        /** This device started or stopped transcoding for other members (mc_sync_transcoding()). */
        data class Transcoding(val active: Boolean) : Event
        /** Every other device left the group or was removed, so the group is gone and this device, [deviceName], is in none. Once per group. */
        data class GroupDissolved(val deviceName: String) : Event
        /**
         * What's known of [uid]'s version changed (refused for it, or synced since): read
         * [CoreSync.devices] and [CoreSync.nearby] again for their versionText.
         */
        data class Version(val uid: String) : Event

        /** What the next sync would copy changed: read [CoreSync.outlook] again. */
        data object Outlook : Event
    }

    internal var handle = nativeNew(library.handle, configDir, deviceName, kind.ordinal, this)

    val deviceUid: String = nativeDeviceUid(handle)

    /**
     * What the group calls this device. Setting it renames it for the whole group (trimmed;
     * ERR_INVALID_ARG if blank); the group may rename it too, so it can change after a session.
     * Save it to pass to the constructor next time.
     */
    var deviceName: String
        get() = nativeGetDeviceName(handle)
        set(value) = nativeSetDeviceName(handle, value)

    /** The TCP port sync listens on once started, else 0. */
    val port: Int get() = nativePort(handle)

    /** Listens, starts discovery, and syncs with group members as they appear. */
    fun start() = nativeStart(handle)

    /** Ends sessions and pairings in progress and waits for the core's threads. */
    fun stop() = nativeStop(handle)

    /** Asks [uid], from [nearby], to pair; it shows a code, which goes to [pairEnter]. */
    fun pairRequest(uid: String) = nativeWithDevice(handle, CALL_PAIR_REQUEST, uid)

    /** Asks the device at [host] to pair, for networks that block discovery. [port] 0: the default. */
    fun pairRequestAddress(host: String, port: Int) = nativePairRequestAddress(handle, host, port)

    /** The six-digit code the other device shows. */
    fun pairEnter(code: String) = nativePairEnter(handle, code)

    /** Gives up the pairing in progress, outgoing or incoming. */
    fun pairCancel() = nativePairCancel(handle)

    /** Leaves the group. Blocks for a few seconds at most, telling a member. */
    fun leaveGroup() = nativeLeaveGroup(handle)

    fun removeDevice(uid: String) = nativeWithDevice(handle, CALL_REMOVE_DEVICE, uid)

    /**
     * The Sync button: syncs with [uid], or every reachable member for null, now. It also
     * chooses this device's profile as it stands, if its user hadn't yet: until then nothing is
     * fetched (see "Choosing a profile" in sync.h).
     */
    fun syncNow(uid: String? = null) = nativeWithDevice(handle, CALL_SYNC_NOW, uid)

    /**
     * As [syncNow], for background work: a member synced completely within the last hour, or
     * whose user cancelled a session lately, gets no session until
     * [SyncDevice.autoSyncAfterMs], and then only if sync still runs.
     */
    fun syncBackground(uid: String? = null) = nativeWithDevice(handle, CALL_SYNC_BACKGROUND, uid)

    /**
     * The Cancel button: ends the session with [uid] (every member's for null), whichever side
     * started it, deleting a file it had half fetched, and holds sessions with it back for an
     * hour (MC_SYNC_CANCEL_HOLD_MS) or until [syncNow]. Its [Event.SyncDone]
     * has [Event.SyncDone.cancelled] set. The other device, if its version knows how, is told:
     * its [Event.SyncDone] has [Event.SyncDone.peerCancelled] set, and it starts no automatic
     * session with this one for 10 minutes (MC_SYNC_PEER_CANCEL_HOLD_MS) unless its user
     * presses Sync; it still serves the sessions this one starts.
     */
    fun cancel(uid: String? = null) = nativeWithDevice(handle, CALL_CANCEL, uid)

    /** The library changed here, so members should hear of it soon. Any thread. */
    fun notifyChanged() = nativeNotifyChanged(handle)

    /**
     * Whether this device starts sessions and reach checks by itself (true, the default). Off,
     * sessions under way go on, the ones other devices start are still served, and [syncNow]
     * still starts one; whatever falls due waits until it's on again. It
     * lasts across [stop] and [start]. For an app that syncs by itself only while on screen.
     */
    fun setAutomatic(on: Boolean) = nativeSetAutomatic(handle, on)

    /**
     * Whether this device is on power (charging) and may transcode for other members now
     * (mc_sync_set_power()). A phone starts as neither. Withdrawing [mayTranscode] stops at
     * once: encodes still to come are dropped, and members count those tracks as missing.
     * Returns at once. Any thread.
     */
    fun setPower(onPower: Boolean, mayTranscode: Boolean) = nativeSetPower(handle, onPower, mayTranscode)

    val libraryState: LibraryState get() = LibraryState.entries[nativeLibraryState(handle)]

    /** The group's members, this device included, by name. Empty when not in a group. */
    fun devices(): List<SyncDevice> = nativeListDevices(handle).asList()

    /** Devices discovery sees that aren't in the group, by name. */
    fun nearby(): List<NearbyDevice> = nativeListNearby(handle).asList()

    /** The tracks waiting for a device this one can't reach now, as sync last worked them out. Doesn't block. */
    fun waiting(): SyncWaiting = nativeWaiting(handle)

    /** What the next sync would copy, as sync last worked it out. Doesn't block. */
    fun outlook(): SyncOutlook = nativeOutlook(handle)

    /** What the last session that moved or deleted files did. Reads sync.db. */
    fun lastActivity(): SyncActivity = nativeLastActivity(handle)

    /** What's wrong with the files here, as sync last worked it out. Doesn't block. */
    fun problems(): SyncProblems = nativeProblems(handle)

    /** Forgets the notices among the issues, once the user has seen them. [Event.Waiting] follows. */
    fun dismissNotices() = nativeDismissNotices(handle)

    /** The network changed: reopens discovery and tries members again now. Any thread; returns at once. */
    fun networkChanged() = nativeNetworkChanged(handle)

    /** The offer to take over from a lost archive, or null if none. Blocks for a read of every track. */
    fun archiveOffer(): SyncArchiveOffer? = nativeArchiveOffer(handle)

    /** The one-time dialog for [uid]'s offer was shown: never again as a popup. */
    fun archiveOfferSeen(uid: String) = nativeArchiveOfferSeen(handle, uid)

    /**
     * Starts making this device the archive in place of [uid]: ERR_NOT_FOUND unless it's on
     * offer, ERR_LOCKED without room or while one runs. Returns at once; progress comes as an
     * issue ([SyncIssueKind.Takeover]).
     */
    fun takeOverArchive(uid: String) = nativeTakeOverArchive(handle, uid)

    /**
     * Removes those of [uids] that are lost as of now from the library, so from every device,
     * and returns how many went. Nothing can fetch them back: call it only once the user has
     * confirmed. Throws [ERR_LOCKED] while a session's file phase runs.
     */
    fun removeLost(uids: List<String>): Int = nativeRemoveLost(handle, uids.toTypedArray())

    /**
     * [uid]'s profile, any member's, this device's included. One with none stored yet reads as
     * an editor should start: everything, originals, no budget, no name. ERR_NOT_FOUND if not
     * in a group or [uid] isn't a member.
     */
    fun profile(uid: String): SyncProfile = nativeGetProfile(handle, uid)

    /** What [uid]'s profile syncs in one sentence (mc_sync_profile_summary()): "3 albums and 2 playlists". */
    fun profileSummary(uid: String): String = nativeProfileSummary(handle, uid)

    /**
     * Stores [uid]'s profile, writing only what differs from the stored one, so read it with
     * [profile], change it and store it straight away. [SyncProfile.archive] is ignored. Only
     * this device's own: ERR_LOCKED for another device's, and for an archive's mode, format or
     * budget switch; ERR_INVALID_ARG for a bitrate not in [OPUS_BITRATES].
     */
    fun setProfile(uid: String, profile: SyncProfile) = nativeSetProfile(
        handle,
        uid,
        profile.name,
        profile.syncAll,
        profile.opusKbps,
        profile.budgetEnabled,
        profile.budgetBytes,
        profile.itemKinds(),
        profile.itemUids(),
    )

    /**
     * Makes this device ([uid]) an archive, or stops it being one: only a device sets its own
     * role (ERR_LOCKED for another's). It may stop only while another archive has verified it
     * holds everything (ERR_LOCKED otherwise).
     */
    fun setArchive(uid: String, archive: Boolean) = nativeSetArchive(handle, uid, archive)

    /**
     * How much [uid] would hold with [profile], which needn't be stored; [SyncProfile.archive]
     * previews making it an archive. Blocks for a read of every track. ERR_LIBRARY_MISMATCH if
     * the open library isn't the group's.
     */
    fun estimate(uid: String, profile: SyncProfile): SyncEstimate {
        val out = LongArray(9 + 1 + OPUS_BITRATES.size)
        nativeEstimate(
            handle,
            uid,
            profile.name,
            profile.archive,
            profile.syncAll,
            profile.opusKbps,
            profile.budgetEnabled,
            profile.budgetBytes,
            profile.itemKinds(),
            profile.itemUids(),
            out,
        )
        return SyncEstimate(
            tracks = out[0].toInt(),
            totalBytes = out[1],
            kbps = out[2].toInt(),
            archive = out[3] != 0L,
            overBudget = out[4] != 0L,
            shortfallBytes = out[5],
            limitBytes = out[6],
            freeBytes = out[7],
            importsDropped = out[8].toInt(),
            stepBytes = out.drop(9),
        )
    }

    /**
     * Checks this device's files, [deep]ly by re-hashing them, else by size and time, then
     * reports them to the group. Blocks for as long as that takes, after waiting for a
     * session's file phase to finish.
     */
    fun verify(deep: Boolean): VerifyResult {
        val out = LongArray(4)
        nativeVerify(handle, deep, out)
        return VerifyResult(out[0], out[1], out[2], out[3])
    }

    /**
     * Goes on with the weekly deep check for up to [budgetMs], where it left off, and returns
     * true once it's over for this week. Does nothing if it isn't due. Blocks for that long, but
     * holds up no other sync call. For a job that needs a charger.
     */
    fun verifyWeekly(budgetMs: Long): Boolean = nativeVerifyWeekly(handle, budgetMs)

    /**
     * Who this device is transcoding for now, and how many tracks are queued or being encoded
     * for each, those under way first (mc_sync_transcoding_for()). A uid of "" stands for
     * tracks that didn't say. Empty between the encodes of a session that asks one at a time.
     */
    fun transcodingFor(): List<TranscodeTarget> {
        val uids = arrayOfNulls<String>(MAX_TARGETS)
        val tracks = IntArray(MAX_TARGETS)
        val n = nativeTranscodingFor(handle, uids, tracks)
        return List(n) { TranscodeTarget(uids[it].orEmpty(), tracks[it]) }
    }

    private fun SyncProfile.itemKinds() = IntArray(items.size) { items[it].kind.ordinal }

    private fun SyncProfile.itemUids() = Array(items.size) { items[it].uid }

    /** Stops if started, then frees. */
    override fun close() {
        if (handle != 0L) nativeFree(handle)
        handle = 0
    }

    // Called from jni_bridge.c on the core's threads.
    @Suppress("unused")
    private fun onPeer(uid: String, online: Boolean, member: Boolean) = listener(Event.Peer(uid, online, member))

    @Suppress("unused")
    private fun onPairCode(code: String, requester: String, attemptsLeft: Int) =
        listener(Event.PairCode(code, requester, attemptsLeft))

    @Suppress("unused")
    private fun onPairResult(
        uid: String,
        name: String,
        initiator: Boolean,
        reason: Int,
        attemptsLeft: Int,
        status: Int,
        joined: Boolean,
        archiveName: String,
        archives: Int,
        versionText: String,
    ) = listener(
        Event.PairResult(
            uid, name, initiator, PairReason.entries.getOrElse(reason) { PairReason.Failed }, attemptsLeft, status,
            joined, archiveName, archives, versionText,
        ),
    )

    @Suppress("unused")
    private fun onWaiting() = listener(Event.Waiting)

    @Suppress("unused")
    private fun onTranscoding(active: Boolean) = listener(Event.Transcoding(active))
    private fun onGroupDissolved(deviceName: String) = listener(Event.GroupDissolved(deviceName))
    private fun onVersion(uid: String) = listener(Event.Version(uid))
    private fun onOutlook() = listener(Event.Outlook)

    @Suppress("unused")
    private fun onProgress(
        uid: String,
        phase: Int,
        rowsSent: Long,
        rowsReceived: Long,
        filesDone: Int,
        filesTotal: Int,
        bytesReceived: Long,
        bytesSent: Long,
        sendsDone: Int,
        sendsTotal: Int,
        pace: String,
        user: Boolean,
        connected: Boolean,
    ) = listener(
        Event.Progress(
            uid, Phase.entries[phase], rowsSent, rowsReceived, filesDone, filesTotal, bytesReceived, bytesSent,
            sendsDone, sendsTotal, pace, user, connected,
        ),
    )

    @Suppress("unused")
    private fun onSyncDone(
        uid: String,
        status: Int,
        librarySynced: Boolean,
        converged: Boolean,
        rowsSent: Long,
        rowsReceived: Long,
        filesReceived: Int,
        filesMissing: Int,
        filesCorrupt: Int,
        filesDeleted: Int,
        filesUnknown: Int,
        filesSent: Int,
        bytesReceived: Long,
        bytesSent: Long,
        coversReceived: Int,
        cancelled: Boolean,
        peerCancelled: Boolean,
        user: Boolean,
        connected: Boolean,
        finishedText: String,
    ) = listener(
        Event.SyncDone(
            uid, status, librarySynced, converged, rowsSent, rowsReceived, filesReceived, filesMissing,
            filesCorrupt, filesDeleted, filesUnknown, filesSent, bytesReceived, bytesSent, coversReceived,
            cancelled, peerCancelled, user, connected, finishedText,
        ),
    )

    companion object {
        /** The TCP port sync listens on unless it's taken (MC_SYNC_DEFAULT_PORT). */
        const val DEFAULT_PORT = 47823

        /** The Opus bitrates a profile may use, highest first (mc_opus_bitrates). */
        val OPUS_BITRATES = listOf(256, 192, 160, 128, 96, 64)

        // mc_status values the pairing and profile calls fail with.
        const val ERR_INVALID_ARG = -2
        const val ERR_NOT_FOUND = -8
        const val ERR_LOCKED = -9
        const val ERR_LIBRARY_MISMATCH = -11

        /** MC_ERR_VERSION: the other device's Kithsong can't sync with this one's; its versionText says which to update. */
        const val ERR_VERSION = -17

        // The most members transcodingFor() lists.
        private const val MAX_TARGETS = 16

        // nativeWithDevice's calls
        private const val CALL_PAIR_REQUEST = 0
        private const val CALL_REMOVE_DEVICE = 1
        private const val CALL_SYNC_NOW = 2
        private const val CALL_SYNC_BACKGROUND = 3
        private const val CALL_CANCEL = 4

        init {
            System.loadLibrary("kithsong_jni")
        }

        @JvmStatic private external fun nativeNew(
            library: Long,
            configDir: String,
            deviceName: String,
            kind: Int,
            listener: CoreSync,
        ): Long
        @JvmStatic private external fun nativeFree(handle: Long)
        @JvmStatic private external fun nativeDeviceUid(handle: Long): String
        @JvmStatic private external fun nativeGetDeviceName(handle: Long): String
        @JvmStatic private external fun nativeSetDeviceName(handle: Long, name: String)
        @JvmStatic private external fun nativeStart(handle: Long)
        @JvmStatic private external fun nativeStop(handle: Long)
        @JvmStatic private external fun nativePort(handle: Long): Int
        @JvmStatic private external fun nativeWithDevice(handle: Long, call: Int, uid: String?)
        @JvmStatic private external fun nativePairRequestAddress(handle: Long, host: String, port: Int)
        @JvmStatic private external fun nativePairEnter(handle: Long, code: String)
        @JvmStatic private external fun nativePairCancel(handle: Long)
        @JvmStatic private external fun nativeLeaveGroup(handle: Long)
        @JvmStatic private external fun nativeNotifyChanged(handle: Long)
        @JvmStatic private external fun nativeSetAutomatic(handle: Long, on: Boolean)
        @JvmStatic private external fun nativeSetPower(handle: Long, onPower: Boolean, mayTranscode: Boolean)
        @JvmStatic private external fun nativeLibraryState(handle: Long): Int
        @JvmStatic private external fun nativeListDevices(handle: Long): Array<SyncDevice>
        @JvmStatic private external fun nativeListNearby(handle: Long): Array<NearbyDevice>
        @JvmStatic private external fun nativeWaiting(handle: Long): SyncWaiting
        @JvmStatic private external fun nativeOutlook(handle: Long): SyncOutlook
        @JvmStatic private external fun nativeLastActivity(handle: Long): SyncActivity
        @JvmStatic private external fun nativeProfileSummary(handle: Long, uid: String): String
        @JvmStatic private external fun nativeProblems(handle: Long): SyncProblems
        @JvmStatic private external fun nativeRemoveLost(handle: Long, uids: Array<String>): Int
        @JvmStatic private external fun nativeDismissNotices(handle: Long)
        @JvmStatic private external fun nativeNetworkChanged(handle: Long)
        @JvmStatic private external fun nativeArchiveOffer(handle: Long): SyncArchiveOffer?
        @JvmStatic private external fun nativeArchiveOfferSeen(handle: Long, uid: String)
        @JvmStatic private external fun nativeTakeOverArchive(handle: Long, uid: String)
        @JvmStatic private external fun nativeGetProfile(handle: Long, uid: String): SyncProfile
        @JvmStatic private external fun nativeSetProfile(
            handle: Long,
            uid: String,
            name: String?,
            syncAll: Boolean,
            kbps: Int,
            budgetEnabled: Boolean,
            budgetBytes: Long,
            kinds: IntArray,
            uids: Array<String>,
        )
        @JvmStatic private external fun nativeSetArchive(handle: Long, uid: String, archive: Boolean)
        @JvmStatic private external fun nativeEstimate(
            handle: Long,
            uid: String,
            name: String?,
            archive: Boolean,
            syncAll: Boolean,
            kbps: Int,
            budgetEnabled: Boolean,
            budgetBytes: Long,
            kinds: IntArray,
            uids: Array<String>,
            out: LongArray,
        )
        @JvmStatic private external fun nativeVerify(handle: Long, deep: Boolean, out: LongArray)
        @JvmStatic private external fun nativeVerifyWeekly(handle: Long, budgetMs: Long): Boolean
        @JvmStatic private external fun nativeTranscodingFor(handle: Long, uids: Array<String?>, tracks: IntArray): Int
    }
}
