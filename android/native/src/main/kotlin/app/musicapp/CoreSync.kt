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
) {
    val kind: DeviceKind get() = DeviceKind.entries.getOrElse(kindCode) { DeviceKind.Desktop }
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
    enum class Phase { Connecting, Group, Library, Done }

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

        data class Progress(val uid: String, val phase: Phase, val rowsSent: Long, val rowsReceived: Long) : Event

        /** A session ended; [status] is 0 on success. [librarySynced]: library data was exchanged too. */
        data class SyncDone(
            val uid: String,
            val status: Int,
            val librarySynced: Boolean,
            val rowsSent: Long,
            val rowsReceived: Long,
        ) : Event
    }

    private var handle = nativeNew(library.handle, configDir, deviceName, kind.ordinal, this)

    /** This device's uid. */
    val deviceUid: String = nativeDeviceUid(handle)

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

    /** Syncs with [uid], or every reachable member for null, as soon as possible. */
    fun syncNow(uid: String? = null) = nativeWithDevice(handle, CALL_SYNC_NOW, uid)

    /** The library changed here, so members should hear of it soon. Any thread. */
    fun notifyChanged() = nativeNotifyChanged(handle)

    val libraryState: LibraryState get() = LibraryState.entries[nativeLibraryState(handle)]

    /** The group's members, this device included, by name. Empty when not in a group. */
    fun devices(): List<SyncDevice> = nativeListDevices(handle).asList()

    /** Devices discovery sees that aren't in the group, by name. */
    fun nearby(): List<NearbyDevice> = nativeListNearby(handle).asList()

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
    private fun onProgress(uid: String, phase: Int, rowsSent: Long, rowsReceived: Long) =
        listener(Event.Progress(uid, Phase.entries[phase], rowsSent, rowsReceived))

    @Suppress("unused")
    private fun onSyncDone(uid: String, status: Int, librarySynced: Boolean, rowsSent: Long, rowsReceived: Long) =
        listener(Event.SyncDone(uid, status, librarySynced, rowsSent, rowsReceived))

    companion object {
        /** The TCP port sync listens on unless it's taken (MC_SYNC_DEFAULT_PORT). */
        const val DEFAULT_PORT = 47823

        // mc_status values the pairing calls fail with.
        const val ERR_NOT_FOUND = -8
        const val ERR_LOCKED = -9

        // nativeWithDevice's calls
        private const val CALL_PAIR_REQUEST = 0
        private const val CALL_REMOVE_DEVICE = 1
        private const val CALL_SYNC_NOW = 2

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
    }
}
