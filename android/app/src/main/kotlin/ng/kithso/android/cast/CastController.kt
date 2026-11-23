package ng.kithso.android.cast

import android.content.Context
import android.net.wifi.WifiManager
import android.os.PowerManager
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
import com.google.android.gms.cast.CastDevice as RemoteDevice
import com.google.android.gms.cast.CastStatusCodes
import com.google.android.gms.cast.HlsSegmentFormat
import com.google.android.gms.cast.HlsVideoSegmentFormat
import com.google.android.gms.cast.MediaInfo
import com.google.android.gms.cast.MediaLoadRequestData
import com.google.android.gms.cast.MediaMetadata
import com.google.android.gms.cast.MediaStatus
import com.google.android.gms.cast.framework.CastContext
import com.google.android.gms.cast.framework.CastSession
import com.google.android.gms.cast.framework.SessionManagerListener
import com.google.android.gms.cast.framework.media.RemoteMediaClient
import com.google.android.gms.common.ConnectionResult
import com.google.android.gms.common.GoogleApiAvailability
import kotlinx.coroutines.CancellationException
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
import ng.kithso.android.R
import ng.kithso.android.data.Track
import ng.kithso.android.playback.CastControl
import ng.kithso.android.playback.CastDevice
import ng.kithso.android.playback.toTrack
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
 * Google Cast for the app: finding devices, the cast session, and the phone's sound going to the
 * device. Cast needs Google Play services, so this checks for it first, off the startup path:
 * without it (or if Cast won't start), [available] stays false and nothing else happens, and
 * nothing is logged but that once.
 *
 * Casting, the phone goes on playing as ever, muted, and the device plays its sound as one live
 * stream through the Default Media Receiver, titled only with the app's name: the queue, skips,
 * shuffle, plays and scrobbles all stay the phone's, and the receiver has nothing to skip to. A
 * device with a screen gets the stream as video ([HlsStream]), which shows what's playing, at a
 * quality the network takes, or with too slow a network, only the sound as AAC; a device without
 * a screen, or that won't play that, gets only the sound ([WavServer]). The core PlaybackService [attach]es is told to send its sound to the device
 * ([CoreMediaPlayer.remoteOutput]), and the media session then offers the device's volume to the
 * volume keys. A wake lock and a Wi-Fi lock are held while casting, so the stream keeps going
 * with the screen off, paused too. A cast whose phone stays paused for three minutes ends, and
 * playback stops as [ng.kithso.android.playback.Playback.stop] stops it. However a cast ends, by
 * [disconnect], the device going or the timeout, the phone is left paused where it was.
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
    private var deviceMuted = false

    private var castContext: CastContext? = null
    private var router: MediaRouter? = null
    private var selector: MediaRouteSelector? = null
    private var discovering = false

    // From PlaybackService while it runs; read by the stream's thread too.
    @Volatile private var core: CoreMediaPlayer? = null

    // The connected session, and once casting, the stream and the receiver's client.
    private var session: CastSession? = null
    private var server: CastStream? = null
    private var client: RemoteMediaClient? = null
    // The receiver has taken the stream; till then its media may still be another sender's.
    private var loaded = false
    // The receiver has got as far as playing the stream.
    private var played = false
    // Another app's media has replaced the stream on the device.
    private var displaced = false
    // What the receiver last said it was doing, to tell when that changes.
    private var receiverState = MediaStatus.PLAYER_STATE_UNKNOWN
    private var starting: Job? = null
    private val paused = MutableStateFlow(true)
    private var pauseWatch: Job? = null
    // What the core's on, for the video's picture.
    private val track = MutableStateFlow<Track?>(null)
    private var pictures: Job? = null

    private val wakeLock = (context.getSystemService(Context.POWER_SERVICE) as PowerManager)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kithsong:cast")
        .apply { setReferenceCounted(false) }
    @Suppress("DEPRECATION") // LOW_LATENCY only holds while the screen's on
    // Null on a device without Wi-Fi (an Ethernet-only TV box), where there's nothing to keep awake.
    private val wifiLock = context.applicationContext.getSystemService(WifiManager::class.java)
        ?.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "kithsong:cast")
        ?.apply { setReferenceCounted(false) }

    // The device, as the core's player sees it while casting: its volume for the volume keys.
    private val output = object : CoreMediaPlayer.RemoteOutput {
        override val volume: Float get() = volumeState
        override val muted: Boolean get() = deviceMuted
        override fun setVolume(volume: Float) = this@CastController.setVolume(volume)
        override fun setMuted(muted: Boolean) {
            try {
                session?.setMute(muted)
                deviceMuted = muted
            } catch (e: Exception) {
                Log.w(TAG, "Couldn't mute the cast", e)
            }
            core?.remoteVolumeChanged()
        }
    }

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
            readVolume()
            core?.remoteVolumeChanged()
        }

        override fun onDeviceNameChanged() {
            deviceName = session?.castDevice?.friendlyName
        }
    }

    private val clientCallback = object : RemoteMediaClient.Callback() {
        override fun onStatusUpdated() = onReceiverStatus()
    }

    // The phone's player: whether it's paused, for the timeout, and starting again.
    private val coreListener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) = onCoreChanged(player)
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
        try {
            session?.volume = volume.toDouble().coerceIn(0.0, 1.0)
        } catch (e: Exception) {
            Log.w(TAG, "Couldn't set the cast volume", e)
        }
        // As asked, for a slider being dragged; the device's word follows (onVolumeChanged).
        volumeState = volume.coerceIn(0f, 1f)
        core?.remoteVolumeChanged()
    }

    /** PlaybackService has started, with [core]: a cast already on takes its sound from now. */
    fun attach(core: CoreMediaPlayer) {
        this.core = core
        core.addListener(coreListener)
        if (casting) core.remoteOutput = output
        onCoreChanged(core)
    }

    /** PlaybackService is going, and the core with it: a cast ends first, leaving it paused. */
    fun detach() {
        if (casting || connecting) endCast(stop = false)
        core?.removeListener(coreListener)
        core = null
        paused.value = true
        track.value = null
    }

    private fun updateDevices() {
        val router = router ?: return
        val selector = selector ?: return
        devices = router.routes
            .filter { !it.isDefaultOrBluetooth && it.isEnabled && it.matchesSelector(selector) }
            .map { CastDevice(it.id, it.name, it.description) }
    }

    private fun readVolume() {
        try {
            volumeState = session?.volume?.toFloat() ?: 0f
            deviceMuted = session?.isMute ?: false
        } catch (e: IllegalStateException) {
            volumeState = 0f
        }
    }

    // A session is connected: the stream starts (it opens a socket), then the device plays it.
    private fun started(session: CastSession) {
        if (this.session === session) return
        this.session?.removeCastListener(castListener)
        this.session = session
        session.addCastListener(castListener)
        connecting = true
        deviceName = session.castDevice?.friendlyName
        readVolume()
        val device = session.castDevice
        val video = device != null && device.hasCapability(RemoteDevice.CAPABILITY_VIDEO_OUT)
        startStream(session, if (video) Kind.Video else Kind.Wav) { begin(session, it) }
    }

    // What the device is sent: a video, or only the sound, as AAC or as it is.
    private enum class Kind { Video, Aac, Wav }

    // Makes the stream and gives it to [then]; if it can't, the cast ends.
    private fun startStream(session: CastSession, kind: Kind, then: (CastStream) -> Unit) {
        starting?.cancel()
        starting = scope.launch {
            val address = session.castDevice?.inetAddress
            // Kept outside the IO block, so a server started as this is cancelled still closes:
            // the block runs to the end however it's cancelled, and withContext only throws after.
            val made = AtomicReference<CastStream?>()
            var kept = false
            try {
                withContext(Dispatchers.IO) {
                    made.set(
                        if (kind == Kind.Wav) WavServer.start(context, address, ::readSound)
                        else HlsStream.start(context, address, kind == Kind.Video, ::readSound, ::onTrouble),
                    )
                }
                val server = made.get()
                if (this@CastController.session !== session) return@launch
                if (server == null) {
                    Log.w(TAG, "No address to stream the music from, so the cast ends")
                    castContext?.sessionManager?.endCurrentSession(true)
                    return@launch
                }
                kept = true
                this@CastController.server = server
                then(server)
                // It may have come before it was kept, when [fallBack] didn't know it.
                if (server is HlsStream) server.trouble?.let { fallBack(server, it.name) }
            } finally {
                if (!kept) made.get()?.close()
            }
        }
    }

    // On a thread of the stream's.
    private fun onTrouble(stream: HlsStream) {
        scope.launch(Dispatchers.Main) { fallBack(stream, stream.trouble?.name.orEmpty()) }
    }

    // [stream] can't go on, so the device gets less for the rest of the cast: only the sound, as
    // AAC where it's the network, as that's less to send than it is as it is.
    private fun fallBack(stream: CastStream, why: String) {
        val session = session ?: return
        if (stream !== server || stream !is HlsStream) return
        val slow = stream.trouble == HlsStream.Trouble.TooSlow || stream.trouble == null && stream.reduced
        val kind = if (stream.hasPicture && slow) Kind.Aac else Kind.Wav
        Log.w(TAG, "The cast's stream can't go on ($why), so it goes to $kind")
        pictures?.cancel()
        pictures = null
        stream.close()
        server = null
        loaded = false
        startStream(session, kind) { server -> client?.let { load(it, server) } }
    }

    // The tap's sound for the stream, on its thread; nothing while there's no core or no tap.
    private fun readSound(out: ShortArray, rate: IntArray): Int = core?.readTap(out, rate)?.coerceAtLeast(0) ?: 0

    // The device plays the stream, and the phone's sound goes there.
    private fun begin(session: CastSession, server: CastStream) {
        // [connecting] stays true while connected, as CastControl has it.
        casting = true
        loaded = false
        displaced = false
        receiverState = MediaStatus.PLAYER_STATE_UNKNOWN
        wakeLock.acquire()
        wifiLock?.acquire()
        core?.remoteOutput = output
        client = session.remoteMediaClient?.also {
            it.registerCallback(clientCallback)
            load(it, server)
        }
        if (server is HlsStream && server.hasPicture) {
            pictures?.cancel()
            // A new track before the last one's picture was made takes over from it.
            pictures = scope.launch {
                track.collectLatest {
                    try {
                        server.show(castPicture(context, it, HlsStream.WIDTH, HlsStream.HEIGHT))
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        // The last one stays up.
                        Log.w(TAG, "Couldn't make the cast's picture", e)
                    }
                }
            }
        }
        pauseWatch?.cancel()
        pauseWatch = scope.launch {
            endAfterPaused(paused) {
                Log.i(TAG, "The cast was paused for long enough, so it ends")
                endCast(stop = true)
            }
        }
    }

    private fun load(client: RemoteMediaClient, server: CastStream) {
        loaded = false
        played = false
        receiverState = MediaStatus.PLAYER_STATE_UNKNOWN
        val metadata = MediaMetadata(MediaMetadata.MEDIA_TYPE_GENERIC).apply {
            putString(MediaMetadata.KEY_TITLE, context.getString(R.string.app_name))
        }
        val stream = MediaInfo.Builder(server.url)
            .setStreamType(MediaInfo.STREAM_TYPE_LIVE)
            .setContentType(server.contentType)
            .setMetadata(metadata)
            .apply { if (server is HlsStream) asTransportStream(server) }
            .build()
        val request = MediaLoadRequestData.Builder().setMediaInfo(stream).setAutoplay(true).build()
        try {
            client.load(request).setResultCallback { result ->
                if (this.client !== client || this.server !== server) return@setResultCallback
                if (result.status.isSuccess) {
                    loaded = true
                } else {
                    Log.w(TAG, "The Cast device wouldn't play the stream: ${result.status}")
                    // Not when it's another load that took this one's place.
                    val code = result.status.statusCode
                    if (code != CastStatusCodes.REPLACED && code != CastStatusCodes.CANCELED) fallBack(server, "the device wouldn't take it")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Couldn't send the stream to the Cast device", e)
        }
    }

    private fun MediaInfo.Builder.asTransportStream(stream: HlsStream) {
        setHlsSegmentFormat(HlsSegmentFormat.TS_AAC)
        if (stream.hasPicture) setHlsVideoSegmentFormat(HlsVideoSegmentFormat.MPEG2_TS)
    }

    private fun onCoreChanged(player: Player) {
        track.value = player.currentMediaItem?.toTrack()
        val nowPaused = !player.playWhenReady || player.playbackState == Player.STATE_IDLE ||
            player.playbackState == Player.STATE_ENDED
        val resumed = paused.value && !nowPaused
        paused.value = nowPaused
        // Playing again: the receiver plays too, if it was paused on the device or let go of
        // the stream (it does, after an error).
        val client = client ?: return
        val server = server ?: return
        if (!resumed || !casting) return
        when (client.mediaStatus?.playerState) {
            MediaStatus.PLAYER_STATE_PAUSED -> client.play()
            null, MediaStatus.PLAYER_STATE_IDLE, MediaStatus.PLAYER_STATE_UNKNOWN -> load(client, server)
            else -> Unit
        }
    }

    // What the receiver says it's doing. Paused there (its remote, say), the phone pauses, as
    // it's what plays; another sender's media in its place ends the cast, leaving that playing.
    private fun onReceiverStatus() {
        val client = client ?: return
        val server = server ?: return
        if (!loaded) return
        val id = client.mediaInfo?.contentId
        if (id != null && id != server.url) {
            Log.i(TAG, "Another app cast to the device, so this cast ends")
            displaced = true
            endCast(stop = false)
            return
        }
        val state = client.mediaStatus?.playerState ?: MediaStatus.PLAYER_STATE_UNKNOWN
        if (state == receiverState) return
        val before = receiverState
        receiverState = state
        when (state) {
            // Not from paused, where it's only getting going again.
            MediaStatus.PLAYER_STATE_BUFFERING -> if (before == MediaStatus.PLAYER_STATE_PLAYING && server is HlsStream) server.stalled()
            MediaStatus.PLAYER_STATE_PLAYING -> played = true
            MediaStatus.PLAYER_STATE_PAUSED -> core?.pause()
            MediaStatus.PLAYER_STATE_IDLE ->
                if (!played && client.mediaStatus?.idleReason == MediaStatus.IDLE_REASON_ERROR) fallBack(server, "the device couldn't play it")
            else -> Unit
        }
    }

    // Ends the cast from here, with [stop] stopping the phone's playback too; then the session
    // ends, stopping the receiver if it's still ours.
    private fun endCast(stop: Boolean) {
        val stopDevice = !displaced
        ended(stop)
        castContext?.sessionManager?.endCurrentSession(stopDevice)
    }

    // The session has gone, however it went: the phone is heard again, paused.
    private fun ended(stop: Boolean = false) {
        pauseWatch?.cancel()
        pauseWatch = null
        pictures?.cancel()
        pictures = null
        starting?.cancel()
        starting = null
        client?.unregisterCallback(clientCallback)
        client = null
        server?.close()
        server = null
        if (casting) {
            core?.let {
                it.pause()
                if (stop) it.stop()
                it.remoteOutput = null
            }
        }
        if (wakeLock.isHeld) wakeLock.release()
        if (wifiLock?.isHeld == true) wifiLock.release()
        session?.removeCastListener(castListener)
        session = null
        loaded = false
        connecting = false
        casting = false
        deviceName = null
        volumeState = 0f
        deviceMuted = false
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
