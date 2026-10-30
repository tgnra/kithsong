package ng.kithso.android.playback

import android.util.Log
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.android.AppSettings
import ng.kithso.core.CoreLibrary
import ng.kithso.core.CoreScrobbler
import ng.kithso.core.MusicCoreException

/**
 * Scrobbling for the app: the core's scrobbler ([CoreScrobbler]) for the library in
 * [libraryDir] and the sync.db in [configDir] (the folder [ng.kithso.android.sync.Syncer]
 * syncs), opened on first use. The player passes on what plays ([nowPlaying], [scrobble]),
 * which is dropped while this phone's switch ([AppSettings.scrobble]) is off. The account is
 * the sync group's: signing in here, in the browser ([signIn]), signs in the other devices
 * when they next sync, and one signed in elsewhere arrives here the same way.
 */
class Scrobbling(
    private val libraryDir: File,
    private val configDir: File,
    private val settings: AppSettings,
    private val scope: CoroutineScope,
) {
    /** What the Settings screen shows. */
    data class State(
        val username: String? = null,
        val hasKey: Boolean = true,
        val pending: Int = 0,
        val lastError: Int = CoreScrobbler.STATUS_OK,
        /** A sign-in is waiting for the user to approve it in the browser. */
        val signingIn: Boolean = false,
        /** Of the last sign-in or sign-out, shown until the next one: a status code, or null. */
        val failure: Int? = null,
        /** The last sign-in wasn't approved in time. */
        val timedOut: Boolean = false,
        val enabled: Boolean = true,
        /** The service: [CoreScrobbler.SERVICE_LASTFM] and so on, with its name and addresses. */
        val service: Int = CoreScrobbler.SERVICE_LASTFM,
        /** "" until read. */
        val serviceName: String = "",
        val apiUrl: String = "",
        val authUrl: String = "",
        /** Of the last change of service, shown until the next one: a status code, or null. */
        val serviceFailure: Int? = null,
    )

    private val mutableState = MutableStateFlow(State(enabled = settings.scrobble))
    val state: StateFlow<State> = mutableState

    private val opening = Any()
    private var library: CoreLibrary? = null
    @Volatile private var scrobbler: CoreScrobbler? = null
    private var signIn: Job? = null // on the main thread

    // Opened once, on a background thread; null if the library can't be.
    private fun open(): CoreScrobbler? = scrobbler ?: synchronized(opening) {
        scrobbler ?: try {
            val lib = library ?: CoreLibrary(libraryDir.path).also { library = it }
            CoreScrobbler(lib, configDir.path).also { scrobbler = it }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't start the scrobbler", e)
            null
        }
    }

    private fun post(what: (CoreScrobbler) -> Unit) {
        if (!settings.scrobble) return
        scope.launch(Dispatchers.IO) {
            val sc = open() ?: return@launch
            try {
                what(sc)
            } catch (e: MusicCoreException) {
                Log.w(TAG, "The scrobbler turned a play down", e)
            }
        }
    }

    /** [trackUid] has started playing. */
    fun nowPlaying(trackUid: String) = post { it.nowPlaying(trackUid) }

    /** A play of [trackUid] counts, begun at [startedMs] (wall clock). */
    fun scrobble(trackUid: String, startedMs: Long) = post { it.scrobble(trackUid, startedMs) }

    /** This phone's switch: whether it scrobbles at all. */
    fun setEnabled(on: Boolean) {
        settings.scrobble = on
        mutableState.value = mutableState.value.copy(enabled = on)
    }

    /** Reads the scrobbler's status again: call now and then while it's shown. */
    suspend fun refresh() {
        val status = withContext(Dispatchers.IO) {
            try {
                open()?.status
            } catch (e: MusicCoreException) {
                null
            }
        } ?: return
        mutableState.value = mutableState.value.copy(
            username = status.username,
            hasKey = status.hasKey,
            pending = status.pending,
            lastError = status.lastError,
            service = status.service,
            serviceName = status.serviceName,
            apiUrl = status.apiUrl,
            authUrl = status.authUrl,
        )
    }

    /**
     * Signs in: gets the page to approve the sign-in on and has [openPage] open it in the
     * browser, then waits for the user to approve it there, for up to ten minutes. Call on the
     * main thread.
     */
    fun signIn(openPage: (String) -> Unit) {
        if (signIn?.isActive == true) return
        mutableState.value = mutableState.value.copy(signingIn = true, failure = null, timedOut = false)
        signIn = scope.launch {
            var result = CoreScrobbler.STATUS_OK
            var polled = false
            try {
                val sc = withContext(Dispatchers.IO) { open() } ?: throw MusicCoreException(STATUS_DB)
                val url = withContext(Dispatchers.IO) { sc.authBegin() }
                openPage(url)
                polled = true
                val end = System.currentTimeMillis() + AUTH_LIMIT_MS
                do {
                    delay(AUTH_POLL_MS)
                    result = withContext(Dispatchers.IO) { sc.authFinish() }
                } while (isActive && result == CoreScrobbler.STATUS_AUTH && System.currentTimeMillis() < end)
            } catch (e: MusicCoreException) {
                result = e.status
            }
            mutableState.value = mutableState.value.copy(
                signingIn = false,
                failure = result.takeIf { it != CoreScrobbler.STATUS_OK },
                timedOut = polled && result == CoreScrobbler.STATUS_AUTH,
            )
            refresh()
        }
    }

    /** Stops waiting for a sign-in to be approved. */
    fun cancelSignIn() {
        signIn?.cancel()
        scrobbler?.authCancel() // the call it's blocked in, if any, and its token
        signIn = null
        mutableState.value = mutableState.value.copy(signingIn = false)
    }

    /** Signs out, here and on the group's other devices when they sync. */
    fun signOut() {
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    open()?.signOut()
                    null
                } catch (e: MusicCoreException) {
                    e.status
                }
            }
            mutableState.value = mutableState.value.copy(failure = failure, timedOut = false)
            refresh()
        }
    }

    /**
     * Scrobbles to another service from now on ([CoreScrobbler.setService]: the strings are
     * only for [CoreScrobbler.SERVICE_OTHER]). Changing it signs out, here and on the group's
     * other devices when they sync. A sign-in under way is given up first: it's for the old
     * service. Call on the main thread.
     */
    fun setService(
        service: Int,
        apiUrl: String? = null,
        authUrl: String? = null,
        apiKey: String? = null,
        apiSecret: String? = null,
    ) {
        if (signIn?.isActive == true) cancelSignIn()
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    open()?.setService(service, apiUrl, authUrl, apiKey, apiSecret)
                    null
                } catch (e: MusicCoreException) {
                    e.status
                }
            }
            mutableState.value = mutableState.value.copy(
                failure = null,
                timedOut = false,
                serviceFailure = failure,
            )
            refresh()
        }
    }

    private companion object {
        const val TAG = "kithsong"
        const val STATUS_DB = -5 // MC_ERR_DB: the library or sync.db wouldn't open
        const val AUTH_POLL_MS = 3000L
        const val AUTH_LIMIT_MS = 10 * 60 * 1000L
    }
}
