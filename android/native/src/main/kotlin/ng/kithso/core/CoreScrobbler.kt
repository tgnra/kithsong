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
        /** [SERVICE_LASTFM], [SERVICE_LIBREFM] or [SERVICE_OTHER]. */
        val service: Int,
        /** What to call it: "Last.fm", "Libre.fm", or another's host name. */
        val serviceName: String,
        val apiUrl: String,
        /** Where its users approve a sign-in. */
        val authUrl: String,
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

    /**
     * Gives up on the sign-in under way, from any thread: an [authBegin] or [authFinish]
     * blocked on the network returns within about a second ([STATUS_NET]), and so does every
     * later [authFinish] for its token.
     */
    fun authCancel() = nativeAuthCancel(handle)

    /** Signs out, here and across the group. */
    fun signOut() = nativeSignOut(handle)

    /**
     * Which service to scrobble to: [SERVICE_LASTFM] (the default), [SERVICE_LIBREFM], or
     * [SERVICE_OTHER] at [apiUrl], signed in to on [authUrl] (http:// or https:// addresses)
     * with its own [apiKey] and [apiSecret]; the strings are only for another service.
     * Changing it signs out, here and across the group.
     */
    fun setService(
        service: Int,
        apiUrl: String? = null,
        authUrl: String? = null,
        apiKey: String? = null,
        apiSecret: String? = null,
    ) = nativeSetService(handle, service, apiUrl, authUrl, apiKey, apiSecret)

    val status: Status
        get() {
            val v = LongArray(4)
            val s = nativeStatus(handle, v)
            return Status(s[0], v[0].toInt(), v[1] != 0L, v[2].toInt(), v[3].toInt(), s[1] ?: "", s[2] ?: "", s[3] ?: "")
        }

    override fun close() {
        if (handle != 0L) nativeFree(handle)
        handle = 0
    }

    companion object {
        const val STATUS_OK = 0
        const val STATUS_NET = -6 // MC_ERR_NET
        const val STATUS_AUTH = -12 // MC_ERR_AUTH

        // mc_scrobbler_service
        const val SERVICE_LASTFM = 0
        const val SERVICE_LIBREFM = 1
        const val SERVICE_OTHER = 2

        init {
            System.loadLibrary("kithsong_jni")
        }

        @JvmStatic external fun nativeNew(library: Long, configDir: String): Long
        @JvmStatic external fun nativeFree(handle: Long)
        @JvmStatic external fun nativePost(handle: Long, trackUid: String, startedMs: Long)
        @JvmStatic external fun nativeAuthBegin(handle: Long): String
        @JvmStatic external fun nativeAuthFinish(handle: Long): Int
        @JvmStatic external fun nativeAuthCancel(handle: Long)
        @JvmStatic external fun nativeSignOut(handle: Long)
        @JvmStatic external fun nativeSetService(
            handle: Long,
            service: Int,
            apiUrl: String?,
            authUrl: String?,
            apiKey: String?,
            apiSecret: String?,
        )
        @JvmStatic external fun nativeStatus(handle: Long, out: LongArray): Array<String?>
    }
}
