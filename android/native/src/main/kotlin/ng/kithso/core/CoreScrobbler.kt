package ng.kithso.core

/**
 * musiccore's scrobbler (mc_scrobbler): passes this device's plays on to Last.fm (or a service
 * with the same API) for the sync group's account, kept in sync.db in [configDir], the same
 * folder [CoreSync] is given. [library] must stay open while this exists; it's only read. It
 * may be used from any thread. [authBegin] and [authFinish] block on the network.
 */
class CoreScrobbler(library: CoreLibrary, configDir: String) : AutoCloseable {
    data class Status(
        /** Null when signed out. */
        val username: String?,
        /** Plays queued here and not yet sent. */
        val pending: Int,
        /** False when the service has no API key: Last.fm in a build made without one. */
        val hasKey: Boolean,
        /** Of the last call to the service: [STATUS_AUTH] if it refused the session, [STATUS_NET] if unreachable. */
        val lastError: Int,
    )

    private var handle = nativeNew(library.handle, configDir)

    /** [trackUid] has started playing. Best effort. */
    fun nowPlaying(trackUid: String) = nativePost(handle, trackUid, -1)

    /** A play of [trackUid] counts; [startedMs] is when it began (wall clock). */
    fun scrobble(trackUid: String, startedMs: Long) = nativePost(handle, trackUid, startedMs)

    /** Gets a token: returns the page where the user approves it, to open in the browser. */
    fun authBegin(): String = nativeAuthBegin(handle)

    /**
     * Turns the approved token into a session: [STATUS_OK] once signed in (the account then
     * reaches the rest of the group by sync), [STATUS_AUTH] while not approved yet, so poll it
     * every few seconds; anything else is a failure.
     */
    fun authFinish(): Int = nativeAuthFinish(handle)

    /** Signs out, here and across the group. */
    fun signOut() = nativeSignOut(handle)

    val status: Status
        get() {
            val v = LongArray(3)
            val name = nativeStatus(handle, v)
            return Status(name, v[0].toInt(), v[1] != 0L, v[2].toInt())
        }

    override fun close() {
        if (handle != 0L) nativeFree(handle)
        handle = 0
    }

    companion object {
        const val STATUS_OK = 0
        const val STATUS_NET = -6 // MC_ERR_NET
        const val STATUS_AUTH = -12 // MC_ERR_AUTH

        init {
            System.loadLibrary("kithsong_jni")
        }

        @JvmStatic external fun nativeNew(library: Long, configDir: String): Long
        @JvmStatic external fun nativeFree(handle: Long)
        @JvmStatic external fun nativePost(handle: Long, trackUid: String, startedMs: Long)
        @JvmStatic external fun nativeAuthBegin(handle: Long): String
        @JvmStatic external fun nativeAuthFinish(handle: Long): Int
        @JvmStatic external fun nativeSignOut(handle: Long)
        @JvmStatic external fun nativeStatus(handle: Long, out: LongArray): String?
    }
}
