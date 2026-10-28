package app.musicapp

/** mc_device_kind. */
enum class DeviceKind { Desktop, Phone, Watch }

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
     * last complete one, MC_SYNC_AUTO_INTERVAL_MS); 0 if it may now. See [CoreSync.syncBackground].
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
    private val confirmCode: Int,
) {
    val kind: DeviceKind get() = DeviceKind.entries.getOrElse(kindCode) { DeviceKind.Desktop }

    /** Whether its files sync on their own (see [CoreSync.confirmSyncNow]). */
    val confirm: ConfirmState get() = ConfirmState.entries.getOrElse(confirmCode) { ConfirmState.Unconfirmed }
}

/** mc_sync_confirm_state: files sync automatically only for a profile the user confirmed. */
enum class ConfirmState {
    /** As its user last confirmed it. */
    Confirmed,

    /** Never confirmed (just paired): no files until Sync is pressed. */
    Unconfirmed,

    /** Changed since it was confirmed: paused until Sync is pressed. */
    ProfileChanged,
}

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
) {
    // For jni_bridge.c, which passes the items as parallel kinds and uids.
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
    ) : this(
        name,
        archive,
        syncAll,
        opusKbps,
        budgetEnabled,
        budgetBytes,
        // Kinds this version doesn't know are left out by the core already.
        kinds.indices.map { ProfileItem(ProfileItemKind.entries[kinds[it]], uids[it]) },
    )
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

/** A device discovery sees that isn't in this device's group (mc_sync_nearby). */
data class NearbyDevice(
    val uid: String,
    val name: String,
    private val kindCode: Int,
    /** It must leave its own group before it can pair with this one. */
    val inOtherGroup: Boolean,
    val address: String?,
    val port: Int,
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
 */
class CoreSync(
    library: CoreLibrary,
    configDir: String,
    deviceName: String,
    kind: DeviceKind,
    private val listener: (Event) -> Unit,
) : AutoCloseable {
    /** mc_pair_reason. */
    enum class PairReason { Paired, WrongCode, InGroup, LibraryMismatch, AlreadyPaired, Busy, Cancelled, Failed }

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
         */
        data class PairResult(
            val uid: String,
            val name: String,
            val initiator: Boolean,
            val reason: PairReason,
            val attemptsLeft: Int,
            val status: Int,
        ) : Event

        /**
         * A session's progress. Rows count the whole session so far. In [Phase.Files],
         * [filesDone] of [filesTotal] are the files this device is fetching (received, failed
         * or refused), and the bytes count the whole session's files both ways; they're 0 in
         * the other phases.
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
        ) : Event

        /**
         * A session ended; [status] is 0 on success. [librarySynced]: library data was
         * exchanged too, and then the files counts are set, this device as the receiver:
         * fetched and verified, wanted but still without a file, failed their checksum twice,
         * deleted (no longer wanted, or failed the check after the session), and unknown to
         * the library (deleted, except on an archive). [filesSent] were served to the peer.
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
        ) : Event
    }

    private var handle = nativeNew(library.handle, configDir, deviceName, kind.ordinal, this)

    /** This device's uid. */
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
     * Syncs with [uid], or every reachable member for null, as soon as possible. Automatic, as
     * for background work: it confirms nothing, so unconfirmed profiles fetch no files.
     */
    fun syncNow(uid: String? = null) = nativeWithDevice(handle, CALL_SYNC_NOW, uid)

    /** The Sync button: as [syncNow], and confirms this device's profile and [uid]'s (each member's for null). */
    fun confirmSyncNow(uid: String? = null) = nativeWithDevice(handle, CALL_CONFIRM_SYNC_NOW, uid)

    /**
     * As [syncNow], for background work: a member synced completely within the last hour gets
     * no session until [SyncDevice.autoSyncAfterMs], and then only if sync still runs.
     */
    fun syncBackground(uid: String? = null) = nativeWithDevice(handle, CALL_SYNC_BACKGROUND, uid)

    /** The library changed here, so members should hear of it soon. Any thread. */
    fun notifyChanged() = nativeNotifyChanged(handle)

    val libraryState: LibraryState get() = LibraryState.entries[nativeLibraryState(handle)]

    /** The group's members, this device included, by name. Empty when not in a group. */
    fun devices(): List<SyncDevice> = nativeListDevices(handle).asList()

    /** Devices discovery sees that aren't in the group, by name. */
    fun nearby(): List<NearbyDevice> = nativeListNearby(handle).asList()

    /**
     * [uid]'s profile, any member's, this device's included. One with none stored yet reads as
     * an editor should start: everything, originals, no budget, no name. ERR_NOT_FOUND if not
     * in a group or [uid] isn't a member.
     */
    fun profile(uid: String): SyncProfile = nativeGetProfile(handle, uid)

    /**
     * Stores [uid]'s profile, writing only what differs from the stored one, so read it with
     * [profile], change it and store it straight away. [SyncProfile.archive] is ignored.
     * ERR_LOCKED for an archive's mode, format or budget switch; ERR_INVALID_ARG for a bitrate
     * not in [OPUS_BITRATES].
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
     * Makes [uid] an archive, or stops it being one. Any member may make any device one; only
     * the device itself may stop, and only while another archive has verified it holds
     * everything (ERR_LOCKED otherwise).
     */
    fun setArchive(uid: String, archive: Boolean) = nativeSetArchive(handle, uid, archive)

    /**
     * How much [uid] would hold with [profile], which needn't be stored; [SyncProfile.archive]
     * previews making it an archive. Blocks for a read of every track. ERR_LIBRARY_MISMATCH if
     * the open library isn't the group's.
     */
    fun estimate(uid: String, profile: SyncProfile): SyncEstimate {
        val out = LongArray(8 + 1 + OPUS_BITRATES.size)
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
            stepBytes = out.drop(8),
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
    private fun onPairResult(uid: String, name: String, initiator: Boolean, reason: Int, attemptsLeft: Int, status: Int) =
        listener(Event.PairResult(uid, name, initiator, PairReason.entries[reason], attemptsLeft, status))

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
    ) = listener(
        Event.Progress(uid, Phase.entries[phase], rowsSent, rowsReceived, filesDone, filesTotal, bytesReceived, bytesSent),
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
    ) = listener(
        Event.SyncDone(
            uid, status, librarySynced, converged, rowsSent, rowsReceived, filesReceived, filesMissing,
            filesCorrupt, filesDeleted, filesUnknown, filesSent, bytesReceived, bytesSent,
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

        // nativeWithDevice's calls
        private const val CALL_PAIR_REQUEST = 0
        private const val CALL_REMOVE_DEVICE = 1
        private const val CALL_SYNC_NOW = 2
        private const val CALL_CONFIRM_SYNC_NOW = 3
        private const val CALL_SYNC_BACKGROUND = 4

        init {
            System.loadLibrary("musicapp_jni")
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
        @JvmStatic private external fun nativeLibraryState(handle: Long): Int
        @JvmStatic private external fun nativeListDevices(handle: Long): Array<SyncDevice>
        @JvmStatic private external fun nativeListNearby(handle: Long): Array<NearbyDevice>
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
    }
}
