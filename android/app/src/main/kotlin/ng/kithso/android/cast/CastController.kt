package ng.kithso.android.cast

import android.content.Context
import android.util.Log
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.media3.common.Player
import androidx.mediarouter.media.MediaRouteSelector
import androidx.mediarouter.media.MediaRouter
import com.google.android.gms.cast.Cast
import com.google.android.gms.cast.framework.CastContext
import com.google.android.gms.cast.framework.CastSession
import com.google.android.gms.cast.framework.SessionManagerListener
import com.google.android.gms.common.ConnectionResult
import com.google.android.gms.common.GoogleApiAvailability
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.android.playback.CastControl
import ng.kithso.android.playback.CastDevice
import ng.kithso.playback.CoreMediaPlayer
import java.util.concurrent.atomic.AtomicReference

/** How long a cast may stay paused before it ends: three minutes. */
const val CAST_PAUSED_MS = 3 * 60 * 1000L

/**
 * Calls [onTimeout] once [paused] has said true for [timeoutMs] on end; saying false meanwhile
 * starts the wait again the next time. Once for each time it pauses. Runs until cancelled.
 */
suspend fun endAfterPaused(paused: Flow<Boolean>, timeoutMs: Long = CAST_PAUSED_MS, onTimeout: () -> Unit) {
    paused.distinctUntilChanged().collectLatest { isPaused ->
        if (isPaused) {
            delay(timeoutMs)
            onTimeout()
        }
    }
}

/**
 * Google Cast for the app: finding devices, the cast session, and handing playback over to the
 * device and back. Cast needs Google Play services, so this checks for it first, off the
 * startup path: without it (or if Cast won't start), [available] stays false and nothing else
 * happens, and nothing is logged but that once.
 *
 * Playing on the device is [CastPlayer]'s, which PlaybackService [attach]es the core's player
 * to, and swaps in for the core's in the media session while a cast is on ([swapTo]). A cast
 * that stays paused for three minutes, by whoever paused it, ends, and playback stops as
 * [ng.kithso.android.playback.Playback.stop] stops it. However a cast ends, by [disconnect], the
 * device going, or the timeout, the phone is left with the same queue at the place the device had
 * got to, paused.
 *
 * Use it on the main thread; [scope] must be on it too.
 */
@Stable
class CastController(private val context: Context, private val scope: CoroutineScope) : CastControl {
    override var available by mutableStateOf(false)
        private set
    override var devices by mutableStateOf<List<CastDevice>>(emptyList())
        private set
    override var connecting by mutableStateOf(false)
        private set
    override var casting by mutableStateOf(false)
        private set
    override var deviceName by mutableStateOf<String?>(null)
        private set
    private var volumeState by mutableFloatStateOf(0f)
    override val volume: Float get() = volumeState

    private var castContext: CastContext? = null
    private var router: MediaRouter? = null
    private var selector: MediaRouteSelector? = null
    private var discovering = false

    // From PlaybackService while it runs.
    private var player: CastPlayer? = null
    private var swapTo: ((Player) -> Unit)? = null
    private var core: CoreMediaPlayer? = null

    // The connected session, and the server for it once started.
    private var session: CastSession? = null
    private var server: CastServer? = null
    private var starting: Job? = null
    private val paused = MutableStateFlow(false)
    private var pauseWatch: Job? = null

    /** The cast player while casting, for what the app asks of the player directly (next, previous). */
    val castPlayer: Player? get() = player?.takeIf { it.casting }

    private val sessionListener = object : SessionManagerListener<CastSession> {
        override fun onSessionStarting(session: CastSession) {
            connecting = true
            deviceName = session.castDevice?.friendlyName
        }

        override fun onSessionStarted(session: CastSession, sessionId: String) = started(session)
        override fun onSessionResumed(session: CastSession, wasSuspended: Boolean) = started(session)

        override fun onSessionStartFailed(session: CastSession, error: Int) {
            Log.w(TAG, "Couldn't start casting: $error")
            ended()
        }

        override fun onSessionEnding(session: CastSession) {}
        override fun onSessionEnded(session: CastSession, error: Int) = ended()
        override fun onSessionResuming(session: CastSession, sessionId: String) {
            connecting = true
        }

        override fun onSessionResumeFailed(session: CastSession, error: Int) = ended()

        // The connection dropped for a moment; Cast tries to get it back, and ends it if it can't.
        override fun onSessionSuspended(session: CastSession, reason: Int) {}
    }

    private val castListener = object : Cast.Listener() {
        override fun onVolumeChanged() {
            player?.updateVolume()
            readVolume()
        }

        override fun onDeviceNameChanged() {
            deviceName = session?.castDevice?.friendlyName
        }
    }

    private val routerCallback = object : MediaRouter.Callback() {
        override fun onRouteAdded(router: MediaRouter, route: MediaRouter.RouteInfo) = updateDevices()
        override fun onRouteRemoved(router: MediaRouter, route: MediaRouter.RouteInfo) = updateDevices()
        override fun onRouteChanged(router: MediaRouter, route: MediaRouter.RouteInfo) = updateDevices()
    }

    init {
        // After startup, as finding Play services and starting Cast take a moment.
        scope.launch { start() }
    }

    private fun start() {
        val status = try {
            GoogleApiAvailability.getInstance().isGooglePlayServicesAvailable(context)
        } catch (e: Exception) {
            ConnectionResult.SERVICE_MISSING
        }
        if (status != ConnectionResult.SUCCESS) {
            Log.i(TAG, "No Google Play services ($status), so no casting")
            return
        }
        val task = try {
            CastContext.getSharedInstance(context, ContextCompat.getMainExecutor(context))
        } catch (e: Exception) {
            Log.i(TAG, "Cast didn't start, so no casting", e)
            return
        }
        task.addOnCompleteListener(ContextCompat.getMainExecutor(context)) { result ->
            val cast = if (result.isSuccessful) result.result else null
            if (cast == null) {
                Log.i(TAG, "Cast didn't start, so no casting", result.exception)
                return@addOnCompleteListener
            }
            try {
                castContext = cast
                selector = cast.mergedSelector
                router = MediaRouter.getInstance(context)
                cast.sessionManager.addSessionManagerListener(sessionListener, CastSession::class.java)
                available = true
                cast.sessionManager.currentCastSession?.takeIf { it.isConnected }?.let(::started)
                if (discovering) startDiscovery()
            } catch (e: Exception) {
                Log.i(TAG, "Cast didn't start, so no casting", e)
                castContext = null
                available = false
            }
        }
    }

    override fun startDiscovery() {
        discovering = true
        val router = router ?: return
        val selector = selector ?: return
        // An active scan too, as a picker is open: devices show up sooner.
        router.addCallback(
            selector,
            routerCallback,
            MediaRouter.CALLBACK_FLAG_REQUEST_DISCOVERY or MediaRouter.CALLBACK_FLAG_PERFORM_ACTIVE_SCAN,
        )
        updateDevices()
    }

    override fun stopDiscovery() {
        discovering = false
        router?.removeCallback(routerCallback)
    }

    override fun connect(device: CastDevice) {
        val router = router ?: return
        val route = router.routes.firstOrNull { it.id == device.id } ?: return
        connecting = true
        deviceName = device.name
        // Cast starts a session on the route it's given, and SessionManagerListener hears of it.
        router.selectRoute(route)
    }

    override fun disconnect() = endCast(stop = false)

    override fun setVolume(volume: Float) {
        val player = player
        if (player != null && player.casting) {
            player.setCastVolume(volume.toDouble())
        } else {
            try {
                session?.volume = volume.toDouble().coerceIn(0.0, 1.0)
            } catch (e: Exception) {
                Log.w(TAG, "Couldn't set the cast volume", e)
            }
        }
        // As asked, for a slider being dragged; the device's word follows (onVolumeChanged).
        volumeState = volume.coerceIn(0f, 1f)
    }

    /**
     * PlaybackService has started, with [core] and the session to play it in: [swapTo] puts a
     * player in the session's hands. A cast already connected takes over now.
     */
    fun attach(core: CoreMediaPlayer, plays: CastPlays, swapTo: (Player) -> Unit) {
        this.core = core
        this.swapTo = swapTo
        player = CastPlayer(
            context, core, plays, scope,
            onStop = { endCast(stop = true) },
            onDisplaced = {
                Log.i(TAG, "Another app cast to the device, so this cast ends")
                endCast(stop = false)
            },
        )
        handOver()
    }

    /** Up next changed: the device's next track may have to change with it. */
    fun upNextChanged() {
        player?.takeIf { it.casting }?.queueChanged()
    }

    /** PlaybackService is going: a cast ends with it, the core back where the device got to. */
    fun detach() {
        if (player?.casting == true) endCast(stop = false)
        player?.release()
        player = null
        swapTo = null
        core = null
    }

    private fun updateDevices() {
        val router = router ?: return
        val selector = selector ?: return
        devices = router.routes
            .filter { !it.isDefaultOrBluetooth && it.isEnabled && it.matchesSelector(selector) }
            .map { CastDevice(it.id, it.name, it.description) }
    }

    private fun readVolume() {
        volumeState = try {
            session?.volume?.toFloat() ?: 0f
        } catch (e: IllegalStateException) {
            0f
        }
    }

    // A session is connected: the server starts (it opens a socket), then playback moves over.
    private fun started(session: CastSession) {
        if (this.session === session) return
        this.session?.removeCastListener(castListener)
        this.session = session
        session.addCastListener(castListener)
        connecting = true
        deviceName = session.castDevice?.friendlyName
        readVolume()
        starting?.cancel()
        starting = scope.launch {
            val address = session.castDevice?.inetAddress
            // Kept outside the IO block, so a server started as this is cancelled still closes:
            // the block runs to the end however it's cancelled, and withContext only throws after.
            val made = AtomicReference<CastServer?>()
            var kept = false
            try {
                withContext(Dispatchers.IO) { made.set(CastServer.start(context, address)) }
                val server = made.get()
                if (this@CastController.session !== session) return@launch
                if (server == null) {
                    Log.w(TAG, "No address to serve the music from, so the cast ends")
                    castContext?.sessionManager?.endCurrentSession(true)
                    return@launch
                }
                kept = true
                this@CastController.server = server
                // [connecting] stays true while connected, as CastControl has it.
                casting = true
                handOver()
            } finally {
                if (!kept) made.get()?.close()
            }
        }
    }

    // Playback goes over to the device, if there's both a session and the player to play it.
    private fun handOver() {
        val session = session ?: return
        val server = server ?: return
        val player = player ?: return
        if (player.casting) return
        val client = session.remoteMediaClient ?: return
        player.begin(session, client, server)
        swapTo?.invoke(player)
        paused.value = !player.playWhenReady
        player.addListener(pauseListener)
        pauseWatch?.cancel()
        pauseWatch = scope.launch {
            endAfterPaused(paused) {
                Log.i(TAG, "The cast was paused for long enough, so it ends")
                endCast(stop = true)
            }
        }
    }

    private val pauseListener = object : Player.Listener {
        override fun onPlayWhenReadyChanged(playWhenReady: Boolean, reason: Int) {
            paused.value = !playWhenReady
        }
    }

    // Ends the cast from here: playback comes back to the phone first, where the device had got
    // to, and with [stop], stops there too; then the session ends, stopping the device.
    private fun endCast(stop: Boolean) {
        // The receiver only stops if it's still ours: another app may have cast over it.
        val stopDevice = player?.let { !it.casting || it.ownsDevice } ?: true
        handBack()
        if (stop) core?.stop()
        castContext?.sessionManager?.endCurrentSession(stopDevice)
        ended()
    }

    private fun handBack() {
        pauseWatch?.cancel()
        pauseWatch = null
        val player = player ?: return
        player.removeListener(pauseListener)
        if (!player.casting) return
        player.end()
        core?.let { swapTo?.invoke(it) }
    }

    // The session has gone, however it went.
    private fun ended() {
        handBack()
        starting?.cancel()
        starting = null
        server?.close()
        server = null
        session?.removeCastListener(castListener)
        session = null
        connecting = false
        casting = false
        deviceName = null
        volumeState = 0f
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
