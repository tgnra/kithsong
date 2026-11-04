package ng.kithso.wear.playback

import android.content.ComponentName
import android.content.Context
import android.util.Log
import androidx.annotation.OptIn
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.media3.common.C
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.common.util.Util
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.core.CorePlayer
import ng.kithso.core.NativeCore
import ng.kithso.core.TrackInfo
import java.util.concurrent.ExecutionException

/**
 * The UI's playback: what is playing as Compose state, and the commands, through the media
 * session of [PlaybackService] with a MediaController. So playing goes on after the activity
 * closes, and the watch's own media controls see it. The controller connects in the
 * background; anything asked for before then is done once it has. Use it on the main thread;
 * [scope] must be on it too. Read [positionMs] in as narrow a scope as possible: it changes
 * several times a second while playing.
 */
@OptIn(UnstableApi::class)
class SessionPlayback(context: Context, private val scope: CoroutineScope) {
    /** The queue's current track; null while nothing is queued. */
    var track by mutableStateOf<TrackInfo?>(null)
        private set
    var isPlaying by mutableStateOf(false)
        private set
    var positionMs by mutableLongStateOf(0L)
        private set
    /** The track's length; 0 while unknown. */
    var durationMs by mutableLongStateOf(0L)
        private set
    var hasNext by mutableStateOf(false)
        private set
    // Previous restarts the track before going back, so it always does something.
    val hasPrevious: Boolean get() = track != null

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
        }, context.mainExecutor)
    }

    /**
     * Plays [tracks] (an album's, a playlist's or a group's tracks with a file on the watch, in order) from
     * [track], which is shown as current at once, before the session says so. [origin] says
     * which, for the play log.
     */
    fun play(tracks: List<TrackInfo>, track: TrackInfo, origin: CorePlayer.Origin) {
        val start = tracks.indexOf(track)
        if (start < 0 || track.path == null) return
        this.track = track
        positionMs = 0
        durationMs = track.durationMs.toLong()
        scope.launch {
            // Finding a cover lists the album's folder, so it's done off the main thread, once
            // per album: a playlist's tracks come from several.
            val covers = withContext(Dispatchers.IO) {
                tracks.mapNotNull { it.coverDir }.distinct().associateWith { NativeCore.coverFile(it) }
            }
            val items = tracks.map { it.toMediaItem(it.coverDir?.let(covers::get), origin) }
            withController {
                it.setMediaItems(items, start, 0)
                it.prepare()
                it.play()
            }
        }
    }

    fun toggle() = withController { Util.handlePlayPauseButtonAction(it) }

    fun next() = withController { it.seekToNext() }

    fun previous() = withController { it.seekToPrevious() }

    fun seekTo(positionMs: Long) {
        this.positionMs = positionMs
        withController { it.seekTo(positionMs) }
    }

    private fun withController(action: (MediaController) -> Unit) {
        val controller = controller
        if (controller != null) action(controller) else pending += action
    }

    private fun update(player: Player) {
        track = player.currentMediaItem?.toTrackInfo()
        // As the play/pause button should show it, so it doesn't flicker while starting.
        isPlaying = !Util.shouldShowPlayButton(player)
        hasNext = player.hasNextMediaItem()
        durationMs = player.duration.takeIf { it != C.TIME_UNSET } ?: track?.durationMs?.toLong() ?: 0L
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
