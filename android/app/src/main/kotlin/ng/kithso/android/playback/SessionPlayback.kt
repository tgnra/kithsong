package ng.kithso.android.playback

import android.content.ComponentName
import android.content.Context
import android.util.Log
import androidx.annotation.OptIn
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.common.util.Util
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import ng.kithso.android.KithsongApp
import ng.kithso.android.cast.CastController
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.data.Track
import ng.kithso.core.CorePlayer
import ng.kithso.core.UpNextEntry
import ng.kithso.playback.CoreMediaPlayer
import ng.kithso.playback.path
import ng.kithso.playback.playOrigin
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.ExecutionException

/**
 * [Playback] through the media session of [PlaybackService], with a MediaController. The
 * controller connects in the background; anything asked for before then is done once it has.
 * Casting is [castController]'s, which PlaybackService plays through while a cast is on.
 * Use it on the main thread; [scope] must be on it too.
 */
@OptIn(UnstableApi::class)
class SessionPlayback(private val context: Context, private val scope: CoroutineScope) : Playback {
    override var track by mutableStateOf<Track?>(null)
        private set
    override var isPlaying by mutableStateOf(false)
        private set
    override var positionMs by mutableLongStateOf(0L)
        private set
    override var hasNext by mutableStateOf(false)
        private set
    override var nextTrack by mutableStateOf<Track?>(null)
        private set
    override var previousTrack by mutableStateOf<Track?>(null)
        private set
    override val hasPrevious get() = previousTrack != null
    private var shuffleOn by mutableStateOf(false)
    override val shuffle get() = shuffleOn
    override var station by mutableStateOf(false)
        private set

    override var upNext by mutableStateOf<List<UpNextEntry>>(emptyList())
        private set

    private val app = context.applicationContext as KithsongApp
    // In this process, as the service is; see PlaybackService.
    private val player get() = app.mediaPlayer

    /** Google Cast, which starts in the background; PlaybackService hands playback to it while casting. */
    val castController = CastController(context.applicationContext, scope)
    override val cast: CastControl get() = castController

    // Where previous goes: the cast's player while casting, as it knows how far into the track
    // the device is, else straight to the core's.
    private val direct: Player? get() = castController.castPlayer ?: player
    private var controller: MediaController? = null
    private val pending = ArrayList<(MediaController) -> Unit>()
    private var ticker: Job? = null

    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) = update(player)
    }

    init {
        val token = SessionToken(context, ComponentName(context, PlaybackService::class.java))
        val future = MediaController.Builder(context, token).buildAsync()
        future.addListener({
            val controller = try {
                future.get()
            } catch (e: ExecutionException) {
                Log.e(TAG, "Couldn't connect to the media session", e)
                return@addListener
            }
            this.controller = controller
            controller.addListener(listener)
            update(controller)
            pending.forEach { it(controller) }
            pending.clear()
        }, ContextCompat.getMainExecutor(context))
    }

    override fun play(queue: List<Track>, index: Int, origin: CorePlayer.Origin) {
        val playable = queue.filter { it.path != null }
        if (playable.isEmpty() || (index >= 0 && queue.getOrNull(index)?.path == null)) return
        val items = queueItems(context, playable, origin)
        if (index < 0) {
            // From the start: the player picks the first track, shuffled, so it's shown once
            // the player says which.
            withController {
                it.setMediaItems(items, true)
                it.prepare()
                it.play()
            }
            return
        }
        val start = queue.subList(0, index).count { it.path != null }
        track = playable[start]
        positionMs = 0
        withController {
            it.setMediaItems(items, start, 0)
            it.prepare()
            it.play()
        }
    }

    // An item with only a query, which the session turns into the queue it stands for.
    override fun playSearch(query: String) {
        val metadata = MediaItem.RequestMetadata.Builder().setSearchQuery(query).build()
        val item = MediaItem.Builder().setRequestMetadata(metadata).build()
        withController {
            it.setMediaItem(item)
            it.prepare()
            it.play()
        }
    }

    override fun toggle() = withController { Util.handlePlayPauseButtonAction(it) }

    override fun seekTo(positionMs: Long) {
        this.positionMs = positionMs
        withController { it.seekTo(positionMs) }
    }

    // Straight to the player: a controller shows the timeline's neighbour at once, which with up
    // next about is the wrong track.
    // By the core's order, which Media3's timeline doesn't always know (up next's last track, a
    // station's next not yet looked up); casting, the cast's player follows the core.
    override fun next() = withController { controller -> player?.skipToNext() ?: controller.seekToNext() }

    override fun previous() = withController { (direct ?: it).seekToPrevious() }

    override fun skipToPrevious() = withController { (direct ?: it).seekToPreviousMediaItem() }

    override fun setShuffle(on: Boolean) {
        shuffleOn = on
        withController { it.shuffleModeEnabled = on }
    }

    // Empty until the controller has connected; nothing has played before then.
    override val queueUids: List<String>
        get() = controller?.let { c -> List(c.mediaItemCount) { c.getMediaItemAt(it).mediaId } }.orEmpty()

    override fun stop() = withController { it.stop() }

    override val queueTracks: List<Track>
        get() = controller?.let { c -> List(c.mediaItemCount) { c.getMediaItemAt(it).toTrack() } }.orEmpty()

    override fun updateQueue(tracks: List<Track>) = withController {
        if (tracks.size == it.mediaItemCount) {
            val origin = it.currentMediaItem?.playOrigin ?: CorePlayer.Origin()
            it.replaceMediaItems(0, tracks.size, queueItems(context, tracks, origin))
        }
    }

    override fun clear() {
        track = null
        positionMs = 0
        withController {
            player?.clearUpNext()
            it.stop()
            // An empty queue, as clearMediaItems() asks to remove items, which the player ignores.
            it.setMediaItems(emptyList())
        }
    }

    override suspend fun addUpNext(what: QueueAddition, next: Boolean): Boolean {
        val source = app.library as? CoreLibrarySource ?: return false
        // The player is there once the controller has connected.
        val connected = CompletableDeferred<CoreMediaPlayer?>()
        withController { connected.complete(player) }
        val player = connected.await() ?: return false
        return source.addUpNext(player, what, next, app.settings.queueTracks)
    }

    override fun moveUpNext(id: Int, to: Int) {
        // At once, as a dragged row has to stay where it's dropped; the player's word follows.
        val from = upNext.indexOfFirst { it.id == id }
        if (from >= 0) upNext = upNext.toMutableList().apply { add(to.coerceIn(0, lastIndex), removeAt(from)) }
        player?.moveUpNext(id, to)
    }

    override fun removeUpNext(id: Int) {
        upNext = upNext.filterNot { it.id == id }
        player?.removeUpNext(id)
    }

    override fun clearUpNext() {
        upNext = emptyList()
        player?.clearUpNext()
    }

    /** Up next changed, or what's around it: from the player, on the main thread. */
    fun upNextChanged() {
        val player = player ?: return
        upNext = player.upNext
        nextTrack = player.itemAt(1)?.toTrack()
        previousTrack = player.itemAt(-1)?.toTrack()
    }

    private fun withController(action: (MediaController) -> Unit) {
        val controller = controller
        if (controller != null) action(controller) else pending += action
    }

    private fun update(player: Player) {
        track = player.currentMediaItem?.toTrack()
        // As the play/pause button should show it, so it doesn't flicker while starting.
        isPlaying = !Util.shouldShowPlayButton(player)
        shuffleOn = player.shuffleModeEnabled
        station = player.currentMediaItem?.playOrigin?.context == CorePlayer.PlayContext.Radio
        // The player says: shuffled the core's queue never ends, though the timeline's shuffle
        // order, which holds each track once, does, and a station's goes on past what's known.
        hasNext = player.isCommandAvailable(Player.COMMAND_SEEK_TO_NEXT)
        upNextChanged()
        positionMs = player.currentPosition
        ticker?.cancel()
        ticker = if (!player.isPlaying) null else scope.launch {
            while (isActive) {
                delay(TICK_MS)
                positionMs = player.currentPosition
            }
        }
    }

    private companion object {
        const val TAG = "kithsong"
        const val TICK_MS = 250L
    }
}
